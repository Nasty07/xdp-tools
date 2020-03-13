// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)

/*
 * XDP management utility functions
 *
 * Copyright (C) 2020 Toke Høiland-Jørgensen <toke@redhat.com>
 */

#define _GNU_SOURCE

#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/file.h>
#include <sys/vfs.h>
#include <fcntl.h>
#include <dirent.h>

#include <linux/err.h> /* ERR_PTR */
#include <linux/if_link.h>
#include <linux/magic.h>

#include <bpf/libbpf.h>
#include <bpf/btf.h>
#include <xdp/libxdp.h>
#include <xdp/prog_dispatcher.h>
#include "logging.h"
#include "util.h"

#define XDP_RUN_CONFIG_SEC ".xdp_run_config"

struct xdp_program {
	/* one of prog or prog_fd should be set */
	struct bpf_program *bpf_prog;
	struct bpf_object *bpf_obj;
	struct btf *btf;
	int prog_fd;
	int link_fd;
	char *link_pin_path;
	char *prog_name;
	__u8 prog_tag[BPF_TAG_SIZE];
	__u64 load_time;
	bool from_external_obj;
	unsigned int run_prio;
	unsigned int chain_call_actions; // bitmap
};


static const char *xdp_action_names[] = {
	[XDP_ABORTED] = "XDP_ABORTED",
	[XDP_DROP] = "XDP_DROP",
	[XDP_PASS] = "XDP_PASS",
	[XDP_TX] = "XDP_TX",
	[XDP_REDIRECT] = "XDP_REDIRECT",
};


static bool bpf_is_valid_mntpt(const char *mnt, unsigned long magic)
{
	struct statfs st_fs;

	if (statfs(mnt, &st_fs) < 0)
		return false;
	if ((unsigned long)st_fs.f_type != magic)
		return false;

	return true;
}

static const char *bpf_find_mntpt_single(unsigned long magic, char *mnt,
					 int len, const char *mntpt)
{
	if (bpf_is_valid_mntpt(mntpt, magic)) {
		strncpy(mnt, mntpt, len-1);
		mnt[len-1] = '\0';
		return mnt;
	}

	return NULL;
}

static const char *find_bpffs()
{
	static bool bpf_mnt_cached = false;
	static char bpf_wrk_dir[PATH_MAX];
	static const char *mnt = NULL;
	char *envdir;

	if (bpf_mnt_cached)
		return mnt;

	envdir = secure_getenv(XDP_BPFFS_ENVVAR);
	mnt = bpf_find_mntpt_single(BPF_FS_MAGIC,
				    bpf_wrk_dir,
				    sizeof(bpf_wrk_dir),
				    envdir ?: BPF_DIR_MNT);
	if (!mnt)
		pr_warn("No bpffs found at %s\n", envdir ?: BPF_DIR_MNT);
	else
		bpf_mnt_cached = 1;

	return mnt;
}

static const char *get_bpffs_dir()
{
	static char bpffs_dir[PATH_MAX];
	static bool dir_cached = false;
	static const char *dir;
	const char *parent;
	int err;

	if (dir_cached)
		return dir;

	parent = find_bpffs();
	if (!parent) {
		err = -ENOENT;
		goto err;
	}

	err = check_snprintf(bpffs_dir, sizeof(bpffs_dir), "%s/xdp", parent);
	if (err)
		goto err;

	err = mkdir(bpffs_dir, S_IRWXU);
	if (err && errno != EEXIST) {
		err = -errno;
		goto err;
	}
	dir = bpffs_dir;
	dir_cached = true;
	return dir;
err:
	return ERR_PTR(err);
}

static int xdp_lock_acquire()
{
	int lock_fd, err;
	const char *dir;

	dir = get_bpffs_dir();
	if (IS_ERR(dir))
		return PTR_ERR(dir);

	lock_fd = open(dir, O_DIRECTORY);
	if (lock_fd < 0) {
		err = -errno;
		pr_warn("Couldn't open lock directory at %s: %s\n",
			dir, strerror(-err));
		return err;
	}

	err = flock(lock_fd, LOCK_EX);
	if (err) {
		err = -errno;
		pr_warn("Couldn't flock fd %d: %s\n", lock_fd, strerror(-err));
		close(lock_fd);
		return err;
	}

	pr_debug("Acquired lock from %s with fd %d\n", dir, lock_fd);
	return lock_fd;
}

static int xdp_lock_release(int lock_fd)
{
	int err;

	err = flock(lock_fd, LOCK_UN);
	if (err) {
		err = -errno;
		pr_warn("Couldn't unlock fd %d: %s\n", lock_fd, strerror(-err));
	} else {
		pr_debug("Released lock fd %d\n", lock_fd);
	}
	close(lock_fd);
	return err;
}

static struct btf *xdp_program__btf(struct xdp_program *xdp_prog)
{
	return xdp_prog->btf;
}

void xdp_program__set_chain_call_enabled(struct xdp_program *prog, unsigned int action,
					 bool enabled)
{
	/* FIXME: Should this also update the BTF info? */
	if (enabled)
		prog->chain_call_actions |= (1<<action);
	else
		prog->chain_call_actions &= ~(1<<action);
}

bool xdp_program__chain_call_enabled(struct xdp_program *prog,
				     enum xdp_action action)
{
	return !!(prog->chain_call_actions & (1<<action));
}

unsigned int xdp_program__run_prio(struct xdp_program *prog)
{
	return prog->run_prio;
}

void xdp_program__set_run_prio(struct xdp_program *prog, unsigned int run_prio)
{
	/* FIXME: Should this also update the BTF info? */
	prog->run_prio = run_prio;
}

const char *xdp_program__name(struct xdp_program *prog)
{
	return prog->prog_name;
}

int xdp_program__print_chain_call_actions(struct xdp_program *prog,
					  char *buf,
					  size_t buf_len)
{
	bool first = true;
	char *pos = buf;
	size_t len = 0;
	int i;

	for (i = 0; i <= XDP_REDIRECT; i++) {
		if (xdp_program__chain_call_enabled(prog, i)) {
			if (!first) {
				*pos++ = ',';
				buf_len--;
			} else {
				first = false;
			}
			len = snprintf(pos, buf_len-len, "%s",
				       xdp_action_names[i]);
			pos += len;
			buf_len -= len;
		}
	}
	return 0;
}

static const struct btf_type *
skip_mods_and_typedefs(const struct btf *btf, __u32 id, __u32 *res_id)
{
	const struct btf_type *t = btf__type_by_id(btf, id);

	if (res_id)
		*res_id = id;

	while (btf_is_mod(t) || btf_is_typedef(t)) {
		if (res_id)
			*res_id = t->type;
		t = btf__type_by_id(btf, t->type);
	}

	return t;
}

static bool get_field_int(const char *prog_name, const struct btf *btf,
			  const struct btf_type *def,
			  const struct btf_member *m, __u32 *res)
{
	const struct btf_type *t = skip_mods_and_typedefs(btf, m->type, NULL);
	const char *name = btf__name_by_offset(btf, m->name_off);
	const struct btf_array *arr_info;
	const struct btf_type *arr_t;

	if (!btf_is_ptr(t)) {
		pr_warn("prog '%s': attr '%s': expected PTR, got %u.\n",
			prog_name, name, btf_kind(t));
		return false;
	}

	arr_t = btf__type_by_id(btf, t->type);
	if (!arr_t) {
		pr_warn("prog '%s': attr '%s': type [%u] not found.\n",
			prog_name, name, t->type);
		return false;
	}
	if (!btf_is_array(arr_t)) {
		pr_warn("prog '%s': attr '%s': expected ARRAY, got %u.\n",
			prog_name, name, btf_kind(arr_t));
		return false;
	}
	arr_info = btf_array(arr_t);
	*res = arr_info->nelems;
	return true;
}

static bool get_xdp_action(const char *act_name, unsigned int *act)
{
	const char **name = xdp_action_names;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(xdp_action_names); i++, name++) {
		if (!strcmp(act_name, *name)) {
			*act = i;
			return true;
		}
	}
	return false;
}

/**
 * This function parses the run config information attached to an XDP program.
 *
 * This information is specified using BTF, in a format similar to how
 * BTF-defined maps are done. The definition looks like this:
 *
 * struct {
 *	__uint(priority, 10);
 *	__uint(XDP_PASS, 1);
 * } XDP_RUN_CONFIG(FUNCNAME);
 *
 * The priority is simply an integer that will be used to sort programs as they
 * are attached on the interface (see cmp_xdp_programs() for full sort order).
 * In addition to the priority, the run config can define an integer value for
 * each XDP action. A non-zero value means that execution will continue to the
 * next loaded program if the current program returns that action. I.e., in the
 * above example, any return value other than XDP_PASS will cause the dispatcher
 * to exit with that return code, whereas XDP_PASS means execution will
 * continue.
 *
 * Since this information becomes part of the object file BTF info, it will
 * survive loading into the kernel, and so it can be retrieved for
 * already-loaded programs as well.
 */
static int xdp_parse_run_config(struct xdp_program *xdp_prog)
{
	const struct btf_type *t, *var, *def, *sec = NULL;
	struct btf *btf = xdp_program__btf(xdp_prog);
	int err, nr_types, i, j, vlen, mlen;
	const struct btf_var_secinfo *vi;
	const struct btf_var *var_extra;
	const struct btf_member *m;
	char struct_name[100];
	const char *name;

	if (!btf) {
		pr_debug("No BTF found for program\n");
		return -ENOENT;
	}

	err = check_snprintf(struct_name, sizeof(struct_name), "_%s",
			     xdp_program__name(xdp_prog));
	if (err)
		return err;

	nr_types = btf__get_nr_types(btf);
	for (i = 1; i <= nr_types; i++) {
		t = btf__type_by_id(btf, i);
		if (!btf_is_datasec(t))
			continue;
		name = btf__name_by_offset(btf, t->name_off);
		if (strcmp(name, XDP_RUN_CONFIG_SEC) == 0) {
			sec = t;
			break;
		}
	}

	if (!sec) {
		pr_debug("DATASEC '%s' not found.\n", XDP_RUN_CONFIG_SEC);
		return -ENOENT;
	}

	vlen = btf_vlen(sec);
	vi = btf_var_secinfos(sec);
	for (i = 0; i < vlen; i++, vi++) {
		var = btf__type_by_id(btf, vi->type);
		var_extra = btf_var(var);
		name = btf__name_by_offset(btf, var->name_off);

		if (strcmp(name, struct_name))
			continue;

		if (!btf_is_var(var)) {
			pr_warn("struct '%s': unexpected var kind %u.\n",
				name, btf_kind(var));
			return -EINVAL;
		}
		if (var_extra->linkage != BTF_VAR_GLOBAL_ALLOCATED &&
		    var_extra->linkage != BTF_VAR_STATIC) {
			pr_warn("struct '%s': unsupported var linkage %u.\n",
				name, var_extra->linkage);
			return -EOPNOTSUPP;
		}

		def = skip_mods_and_typedefs(btf, var->type, NULL);
		if (!btf_is_struct(def)) {
			pr_warn("struct '%s': unexpected def kind %u.\n",
				name, btf_kind(var));
			return -EINVAL;
		}
		if (def->size > vi->size) {
			pr_warn("struct '%s': invalid def size.\n", name);
			return -EINVAL;
		}

		mlen = btf_vlen(def);
		m = btf_members(def);
		for (j = 0; j < mlen; j++, m++) {
			const char *mname = btf__name_by_offset(btf, m->name_off);
			unsigned int val, act;

			if (!mname) {
				pr_warn("struct '%s': invalid field #%d.\n", name, i);
				return -EINVAL;
			}
			if (!strcmp(mname, "priority")) {
				if (!get_field_int(struct_name, btf, def, m,
						   &xdp_prog->run_prio))
					return -EINVAL;
				continue;
			} else if(get_xdp_action(mname, &act)) {
				if (!get_field_int(struct_name, btf, def, m,
						   &val))
					return -EINVAL;
				xdp_program__set_chain_call_enabled(xdp_prog, act, val);
			} else {
				pr_warn("Invalid mname: %s\n", mname);
				return -ENOTSUP;
			}
		}
		return 0;
	}

	pr_debug("Couldn't find run order struct %s\n", struct_name);
	return -ENOENT;
}

static struct xdp_program *xdp_program__new()
{
	struct xdp_program *xdp_prog;

	xdp_prog = malloc(sizeof(*xdp_prog));
	if (!xdp_prog)
		return ERR_PTR(-ENOMEM);

	memset(xdp_prog, 0, sizeof(*xdp_prog));

	xdp_prog->prog_fd = -1;
	xdp_prog->link_fd = -1;
	xdp_prog->run_prio = XDP_DEFAULT_RUN_PRIO;
	xdp_prog->chain_call_actions = XDP_DEFAULT_CHAIN_CALL_ACTIONS;

	return xdp_prog;
}

void xdp_program__free(struct xdp_program *xdp_prog)
{
	if (xdp_prog->link_fd >= 0)
		close(xdp_prog->link_fd);
	if (xdp_prog->prog_fd >= 0)
		close(xdp_prog->prog_fd);

	free(xdp_prog->link_pin_path);
	free(xdp_prog->prog_name);

	if (!xdp_prog->from_external_obj) {
		if (xdp_prog->bpf_obj)
			bpf_object__close(xdp_prog->bpf_obj);
		else if (xdp_prog->btf)
			btf__free(xdp_prog->btf);
	}
}

static int xdp_program__fill_from_obj(struct xdp_program *xdp_prog,
				      struct bpf_object *obj,
				      const char *prog_name,
				      bool external)
{
	struct bpf_program *bpf_prog;
	int err;

	if (prog_name)
		bpf_prog = bpf_object__find_program_by_title(obj, prog_name);
	else
		bpf_prog = bpf_program__next(NULL, obj);

	if(!bpf_prog)
		return -ENOENT;

	xdp_prog->prog_name = strdup(bpf_program__name(bpf_prog));
	if (!xdp_prog->prog_name)
		return -ENOMEM;

	xdp_prog->bpf_prog = bpf_prog;
	xdp_prog->bpf_obj = obj;
	xdp_prog->btf = bpf_object__btf(obj);
	xdp_prog->from_external_obj = external;

	err = xdp_parse_run_config(xdp_prog);
	if (err && err != -ENOENT)
		return err;

	return 0;
}

struct xdp_program *xdp_program__from_bpf_obj(struct bpf_object *obj,
					      const char *prog_name)
{
	struct xdp_program *xdp_prog;
	int err;

	xdp_prog = xdp_program__new();
	if (IS_ERR(xdp_prog))
		return xdp_prog;

	err = xdp_program__fill_from_obj(xdp_prog, obj, prog_name, true);
	if (err)
		goto err;

	return xdp_prog;
err:
	xdp_program__free(xdp_prog);
	return ERR_PTR(err);
}

struct xdp_program *xdp_program__open_file(const char *filename,
					   const char *prog_name,
					   struct bpf_object_open_opts *opts)
{
	struct xdp_program *xdp_prog;
	struct bpf_object *obj;
	int err;

	obj = bpf_object__open_file(filename, opts);
	err = libbpf_get_error(obj);
	if (err)
		return ERR_PTR(err);

	xdp_prog = xdp_program__new();
	if (IS_ERR(xdp_prog)) {
		bpf_object__close(obj);
		return xdp_prog;
	}

	err = xdp_program__fill_from_obj(xdp_prog, obj, prog_name, false);
	if (err)
		goto err;

	return xdp_prog;
err:
	xdp_program__free(xdp_prog);
	bpf_object__close(obj);
	return ERR_PTR(err);
}

static int xdp_program__fill_from_fd(struct xdp_program *xdp_prog, int fd)
{
	struct bpf_prog_info info = {};
	__u32 len = sizeof(info);
	struct btf *btf = NULL;
	int err = 0;

	err = bpf_obj_get_info_by_fd(fd, &info, &len);
	if (err) {
		pr_warn("couldn't get program info: %s", strerror(errno));
		err = -errno;
		goto err;
	}

	if (!xdp_prog->prog_name) {
		xdp_prog->prog_name = strdup(info.name);
		if (!xdp_prog->prog_name) {
			err = -ENOMEM;
			pr_warn("failed to strdup program title");
			goto err;
		}
	}

	if (info.btf_id && !xdp_prog->btf) {
		err = btf__get_from_id(info.btf_id, &btf);
		if (err) {
			pr_warn("Couldn't get BTF for ID %ul\n", info.btf_id);
			goto err;
		}
		xdp_prog->btf = btf;
	}

	memcpy(xdp_prog->prog_tag, info.tag, BPF_TAG_SIZE);
	xdp_prog->load_time = info.load_time;
	xdp_prog->prog_fd = fd;

	return 0;
err:
	btf__free(btf);
	return err;
}

struct xdp_program *xdp_program__from_id(__u32 id)
{
	struct xdp_program *xdp_prog = NULL;
	int fd, err;

	fd = bpf_prog_get_fd_by_id(id);
	if (fd < 0) {
		pr_warn("couldn't get program fd: %s", strerror(errno));
		err = -errno;
		goto err;
	}

	xdp_prog = xdp_program__new();
	if (IS_ERR(xdp_prog))
		return xdp_prog;

	err = xdp_program__fill_from_fd(xdp_prog, fd);
	if (err)
		goto err;

	err = xdp_parse_run_config(xdp_prog);
	if (err && err != -ENOENT)
		goto err;

	return xdp_prog;
err:
	free(xdp_prog);
	return ERR_PTR(err);
}

static int cmp_xdp_programs(const void *_a, const void *_b)
{
	const struct xdp_program *a = *(struct xdp_program * const *)_a;
	const struct xdp_program *b = *(struct xdp_program * const *)_b;
	int cmp;

	if (a->run_prio != b->run_prio)
		return a->run_prio < b->run_prio ? -1 : 1;

	cmp = strcmp(a->prog_name, b->prog_name);
	if (cmp)
		return cmp;

	/* Hopefully the two checks above will resolve most comparisons; in
	 * cases where they don't, hopefully the checks below will keep the
	 * order stable.
	 */

	/* loaded before non-loaded */
	if (a->prog_fd >= 0 && b->prog_fd < 0)
		return -1;
	else if (a->prog_fd < 0 && b->prog_fd >= 0)
		return 1;

	/* two unloaded programs - compare by size */
	if (a->bpf_prog && b->bpf_prog) {
		size_t size_a, size_b;

		size_a = bpf_program__size(a->bpf_prog);
		size_b = bpf_program__size(b->bpf_prog);
		if (size_a != size_b)
			return size_a < size_b ? -1 : 1;
	}

	cmp = memcmp(a->prog_tag, b->prog_tag, BPF_TAG_SIZE);
	if (cmp)
		return cmp;

	/* at this point we are really grasping for straws */
	if (a->load_time != b->load_time)
		return a->load_time < b->load_time ? -1 : 1;

	return 0;
}

int xdp_program__get_from_ifindex(int ifindex,
				  struct xdp_program **progs,
				  size_t *num_progs)
{
	struct xdp_link_info xinfo = {};
	struct xdp_program *p;
	__u32 prog_id;
	int err;

	err = bpf_get_link_xdp_info(ifindex, &xinfo, sizeof(xinfo), 0);
	if (err)
		return err;

	if (xinfo.attach_mode == XDP_ATTACHED_SKB)
		prog_id = xinfo.skb_prog_id;
	else
		prog_id = xinfo.drv_prog_id;

	if (!prog_id)
		return -ENOENT;

	p = xdp_program__from_id(prog_id);
	if (IS_ERR(p))
		return PTR_ERR(p);

	/* FIXME: This should figure out whether the loaded program is a
	 * dispatcher, and if it is, go find the component programs and return
	 * those instead.
	 */
	progs[0] = p;
	*num_progs = 1;

	return 0;
}

int xdp_program__load(struct xdp_program *prog)
{
	int err;

	if (prog->prog_fd >= 0)
		return -EEXIST;

	if (!prog->bpf_obj)
		return -EINVAL;

	err = bpf_object__load(prog->bpf_obj);
	if (err)
		return err;

	pr_debug("Loaded XDP program %s, got fd %d\n",
		 xdp_program__name(prog), bpf_program__fd(prog->bpf_prog));

	return xdp_program__fill_from_fd(prog, bpf_program__fd(prog->bpf_prog));
}

static int pin_multiprog(int dispatcher_fd, struct xdp_program **progs,
			 size_t num_progs)
{
	__u32 info_size = sizeof(struct bpf_prog_info);
	struct bpf_prog_info info = {};
	struct xdp_program *prog;
	char pin_path[PATH_MAX];
	const char *bpffs_dir;
	int err = 0, lock_fd, i;

	bpffs_dir = get_bpffs_dir();
	if (IS_ERR(bpffs_dir))
		return PTR_ERR(bpffs_dir);

	err = bpf_obj_get_info_by_fd(dispatcher_fd, &info, &info_size);
	if (err)
		return err;

	lock_fd = xdp_lock_acquire();
	if (lock_fd < 0)
		return lock_fd;

	err = check_snprintf(pin_path, sizeof(pin_path), "%s/dispatch-%d",
			     bpffs_dir, info.id);
	if (err)
		goto out;

	pr_debug("Pinning multiprog fd %d beneath %s\n", dispatcher_fd, pin_path);

	err = mkdir(pin_path, S_IRWXU);
	if (err && errno != EEXIST) {
		err = -errno;
		goto out;
	}

	for (i = 0; i < num_progs; i++) {
		char buf[PATH_MAX];
		prog = progs[i];

		if (prog->link_fd < 0) {
			err = -EINVAL;
			pr_warn("Prog %s not linked\n", xdp_program__name(prog));
			goto err_unpin;
		}

		err = check_snprintf(buf, sizeof(buf), "%s/link-prog%d",
				     pin_path, i);
		if (err)
			goto err_unpin;

		err = bpf_obj_pin(prog->link_fd, buf);
		if (err) {
			pr_warn("Couldn't pin link FD at %s: %s\n", buf, strerror(-err));
			goto err_unpin;
		}

		prog->link_pin_path = strdup(pin_path);
		if (!prog->link_pin_path) {
			err = -ENOMEM;
			unlink(pin_path);
			goto err_unpin;
		}

		pr_debug("Pinned link for prog %s at %s\n",
			 xdp_program__name(prog), buf);
	}
out:
	xdp_lock_release(lock_fd);
	return err;

err_unpin:
	for (; i >= 0; i--) {
		if (progs[i]->link_pin_path) {
			unlink(progs[i]->link_pin_path);
			free(progs[i]->link_pin_path);
			progs[i]->link_pin_path = NULL;
		}
	}
	goto out;
}

static int unpin_multiprog(int dispatcher_fd)
{
	__u32 info_size = sizeof(struct bpf_prog_info);
	struct bpf_prog_info info = {};
	char pin_path[PATH_MAX];
	const char *bpffs_dir;
	int err = 0, lock_fd;
	struct dirent *de;
	DIR *dr;

	bpffs_dir = get_bpffs_dir();
	if (IS_ERR(bpffs_dir))
		return PTR_ERR(bpffs_dir);

	err = bpf_obj_get_info_by_fd(dispatcher_fd, &info, &info_size);
	if (err)
		return err;

	lock_fd = xdp_lock_acquire();
	if (lock_fd < 0)
		return lock_fd;

	err = check_snprintf(pin_path, sizeof(pin_path), "%s/dispatch-%d",
			     bpffs_dir, info.id);
	if (err)
		goto out;

	dr = opendir(pin_path);
	if (!dr) {
		err = -ENOENT;
		goto out;
	}

	pr_debug("Unpinning multiprog fd %d beneath %s\n",
		 dispatcher_fd, pin_path);

	while ((de = readdir(dr)) != NULL) {
		char buf[PATH_MAX];

		if (!strcmp(".", de->d_name) ||
		    !strcmp("..", de->d_name))
			continue;

		err = check_snprintf(buf, sizeof(buf), "%s/%s",
				     pin_path, de->d_name);
		if (err)
			goto out_dir;

		err = unlink(buf);
		if (err) {
			err = -errno;
			pr_warn("Couldn't unlink file %s: %s\n",
				buf, strerror(-err));
			goto out_dir;
		}
	}

	err = rmdir(pin_path);
	if (err)
		err = -errno;
out_dir:
	closedir(dr);
out:
	xdp_lock_release(lock_fd);
	return err;
}

static int gen_xdp_multiprog(struct xdp_program **progs, size_t num_progs)
{
	struct xdp_dispatcher_config *rodata;
	struct bpf_program *dispatcher_prog;
	int prog_fd, err, i, lfd;
	struct xdp_program *prog;
	struct bpf_object *obj;
	char buf[PATH_MAX];
	size_t sz = 0;

	pr_debug("Generating multi-prog dispatcher for %zu programs\n", num_progs);

	err = find_bpf_file(buf, sizeof(buf), "xdp-dispatcher.o");
	if (err)
		return err;

	obj = bpf_object__open_file(buf, NULL);
	err = libbpf_get_error(obj);
	if (err) {
		pr_warn("Couldn't open BPF file %s\n", buf);
		return err;
	}

	rodata = bpf_object__rodata(obj, &sz);
	if (!rodata) {
		pr_warn("No rodata for object file %s\n", buf);
		err = -ENOENT;
		goto err;
	} else if (sz != sizeof(*rodata)) {
		pr_warn("Object rodata size %zu different from expected %zu\n",
			sz, sizeof(*rodata));
		err = -EINVAL;
		goto err;
	}

	/* set dispatcher parameters before loading */
	rodata->num_progs_enabled = num_progs;
	for (i = 0; i < num_progs; i++)
		rodata->chain_call_actions[i] = progs[i]->chain_call_actions;

	err = bpf_object__load(obj);
	if (err)
		goto err;

	dispatcher_prog = bpf_object__find_program_by_title(obj, "xdp_dispatcher");
	if (!dispatcher_prog) {
		pr_warn("Couldn't find XDP dispatcher program in %s\n", buf);
		err = -ENOENT;
		goto err;
	}

	prog_fd = bpf_program__fd(dispatcher_prog);
	for (i = 0; i < num_progs; i++) {
		prog = progs[i];
		err = check_snprintf(buf, sizeof(buf), "prog%d", i);
		if (err)
			goto err;

		/* FIXME: The following assumes the component XDP programs are
		 * not already loaded. We do want to be able to re-attach
		 * already-loaded programs into a new dispatcher here; but the
		 * kernel doesn't currently allow this. So for now, just assume
		 * programs are not already loaded and load them as TYPE_EXT
		 * programs.
		 */

		err = bpf_program__set_attach_target(prog->bpf_prog, prog_fd,
						     buf);
		if (err) {
			pr_debug("Failed to set attach target: %s\n", strerror(-err));
			goto err;
		}

		bpf_program__set_type(prog->bpf_prog, BPF_PROG_TYPE_EXT);
		err = xdp_program__load(prog);
		if (err) {
			pr_warn("Failed to load program %d ('%s'): %s\n",
				i, xdp_program__name(prog), strerror(-err));
			goto err;
		}

		/* The attach will disappear once this fd is closed */
		lfd = bpf_raw_tracepoint_open(NULL, prog->prog_fd);
		if (lfd < 0) {
			err = lfd;
			pr_warn("Failed to attach program %d ('%s') to dispatcher: %s\n",
				i, xdp_program__name(prog), strerror(-err));
			goto err;
		}

		pr_debug("Attached prog '%s' with priority %d in dispatcher entry '%s' with fd %d\n",
			 xdp_program__name(prog), xdp_program__run_prio(prog),
			 buf, lfd);
		prog->link_fd = lfd;
	}

	return prog_fd;

err:
	bpf_object__close(obj);
	return err;
}

int xdp_attach_programs(struct xdp_program **progs, size_t num_progs,
			int ifindex, bool force, enum xdp_attach_mode mode)
{
	int err = 0, xdp_flags = 0, prog_fd;

	if (!num_progs)
		return -EINVAL;

	if (num_progs > 1) {
		qsort(progs, num_progs, sizeof(*progs), cmp_xdp_programs);
		prog_fd = gen_xdp_multiprog(progs, num_progs);
	} else if (progs[0]->prog_fd >= 0) {
		prog_fd = progs[0]->prog_fd;
	} else {
		prog_fd = xdp_program__load(progs[0]);
	}

	if (prog_fd < 0) {
		err = prog_fd;
		goto out;
	}

	if (num_progs > 1) {
		err = pin_multiprog(prog_fd, progs, num_progs);
		if (err)
			goto out;
	}

	pr_debug("Loading XDP fd %d onto ifindex %d\n", prog_fd, ifindex);

	switch (mode) {
	case XDP_MODE_SKB:
		xdp_flags |= XDP_FLAGS_SKB_MODE;
		break;
	case XDP_MODE_NATIVE:
		xdp_flags |= XDP_FLAGS_DRV_MODE;
		break;
	case XDP_MODE_HW:
		xdp_flags |= XDP_FLAGS_HW_MODE;
		break;
	case XDP_MODE_UNSPEC:
		break;
	}

	if (!force)
		xdp_flags |= XDP_FLAGS_UPDATE_IF_NOEXIST;

	err = bpf_set_link_xdp_fd(ifindex, prog_fd, xdp_flags);
	if (err == -EEXIST && !(xdp_flags & XDP_FLAGS_UPDATE_IF_NOEXIST)) {
		/* Program replace didn't work, probably because a program of
		 * the opposite type is loaded. Let's unload that and try
		 * loading again.
		 */

		__u32 old_flags = xdp_flags;

		xdp_flags &= ~XDP_FLAGS_MODES;
		xdp_flags |= (mode == XDP_MODE_SKB) ? XDP_FLAGS_DRV_MODE :
			XDP_FLAGS_SKB_MODE;
		err = bpf_set_link_xdp_fd(ifindex, -1, xdp_flags);
		if (!err)
			err = bpf_set_link_xdp_fd(ifindex, prog_fd, old_flags);
	}
	if (err < 0) {
		pr_warn("Error attaching XDP program to ifindex %d: %s\n",
			ifindex, strerror(-err));

		switch (-err) {
		case EBUSY:
		case EEXIST:
			pr_warn("XDP already loaded on device;"
				" use --force to replace\n");
			break;
		case EOPNOTSUPP:
			pr_warn("Native XDP not supported;"
				" try using --skb-mode\n");
			break;
		default:
			break;
		}
		goto out;
	}

	pr_debug("Loaded %zu programs on ifindex '%d'%s\n",
		 num_progs, ifindex,
		 mode == XDP_MODE_SKB ? " in skb mode" : "");

	return prog_fd;
out:
	return err;
}


int xdp_program__attach(const struct xdp_program *prog,
			int ifindex, bool replace, enum xdp_attach_mode mode)
{
	struct xdp_program *old_progs[10], *all_progs[10];
	size_t num_old_progs = 10, num_progs;
	int err, i;

	/* FIXME: The idea here is that the API should allow the caller to just
	 * attach a program; and the library will take care of finding the
	 * already-attached programs, inserting the new one into the sequence
	 * based on its priority, build a new dispatcher, and atomically replace
	 * the old one. This needs a kernel API to allow re-attaching already
	 * loaded freplace programs, as well as the ability to attach each
	 * program to multiple places. So for now, this function doesn't really
	 * work.
	 */
	err = xdp_program__get_from_ifindex(ifindex, old_progs, &num_old_progs);
	if (err && err != -ENOENT)
		return err;

	if (replace) {
		num_progs = 1;
		all_progs[0] = (struct xdp_program *)prog;
	} else {
		for (i = 0; i < num_old_progs; i++)
			all_progs[i] = old_progs[i];
		num_progs = num_old_progs +1;
	}

	err = xdp_attach_programs(all_progs, num_progs, ifindex, true, mode);
	if (err)
		return err;
	return 0;
}
