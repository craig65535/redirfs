#include "redir.h"

static spinlock_t flt_list_lock = SPIN_LOCK_UNLOCKED;
static LIST_HEAD(flt_list);
extern path_rem_list;

inline struct filter *flt_get(struct filter *flt)
{
	BUG_ON(!atomic_read(&flt->f_count));
	atomic_inc(&flt->f_count);
	return flt;
}

inline void flt_put(struct filter *flt)
{
	if (!flt || IS_ERR(flt))
		return;

	BUG_ON(!atomic_read(&flt->f_count));
	if (!atomic_dec_and_test(&flt->f_count))
		return;

	kfree(flt->f_name);
	kfree(flt);
}

struct filter *flt_alloc(struct rfs_filter_info *flt_info)
{
	struct filter *flt = NULL;
	char *flt_name = NULL;
	int flt_name_len = 0;

	flt_name_len = strlen(flt_info->name);
	flt = kmalloc(sizeof(struct filter), GFP_KERNEL);
	flt_name = kmalloc(flt_name_len + 1, GFP_KERNEL);

	if (!flt || !flt_name) {
		kfree(flt);
		kfree(flt_name);
		return ERR_PTR(-ENOMEM);
	}

	INIT_LIST_HEAD(&flt->f_list);
	strncpy(flt_name, flt_info->name, flt_name_len);
	flt->f_name = flt_name;
	flt->f_priority = flt_info->priority;
	spin_lock_init(&flt->f_lock);
	atomic_set(&flt->f_count, 1);

	if (flt_info->active)
		atomic_set(&flt->f_active, 1);
	else
		atomic_set(&flt->f_active, 0);

	return flt;
}

int rfs_register_filter(void **filter, struct rfs_filter_info *filter_info)
{
	struct filter *pos = NULL;
	struct filter *flt = flt_alloc(filter_info);

	if (IS_ERR(flt))
		return PTR_ERR(flt);

	spin_lock(&flt_list_lock);

	list_for_each_entry(pos, &flt_list, f_list) {
		if (pos->f_priority == flt->f_priority)
			goto exists;
	}

	flt_get(flt);
	list_add_tail(&flt->f_list, &flt_list);

	spin_unlock(&flt_list_lock);

	*filter = flt;

	return 0;

exists:
	flt_put(flt);
	*filter = NULL;
	return -EEXIST;
}

int rfs_unregister_filter(void *filter)
{
	return 0;
}

enum rfs_err rfs_set_operations(void *filter, struct rfs_op_info ops_info[])
{
	struct filter *flt = (struct filter *)filter;
	int i = 0;
	int retv;

	if (!flt)
		return RFS_ERR_INVAL;

	while (ops_info[i].op_id != RFS_OP_END) {
		flt->f_pre_cbs[ops_info[i].op_id] = ops_info[i].pre_cb;
		flt->f_post_cbs[ops_info[i].op_id] = ops_info[i].post_cb;
		i++;
	}

	mutex_lock(&path_list_mutex);
	retv = path_walk(NULL, flt_set_ops_cb, flt);
	mutex_unlock(&path_list_mutex);

	return retv;
}

int flt_add_local(struct path *path, struct filter *flt)
{
	struct chain *inchain_local = NULL;
	struct chain *exchain_local = NULL;
	struct path *path_go = path;
	struct path *path_cmp = path;
	int retv;

	if (chain_find_flt(path->p_inchain_local, flt) == -1) {
		inchain_local = chain_add_flt(path->p_inchain_local, flt);
		if (IS_ERR(inchain_local))
			return PTR_ERR(inchain_local);

		chain_put(path->p_inchain_local);
		path->p_inchain_local = inchain_local;

		if (chain_find_flt(path->p_exchain_local, flt) != -1) {
			exchain_local = chain_rem_flt(path->p_exchain_local, flt);
			if (IS_ERR(exchain_local))
				return PTR_ERR(exchain_local);

			chain_put(path->p_exchain_local);
			path->p_exchain_local = exchain_local;
		}
	}

	while (path_cmp) {
		if (!(path_cmp->p_flags & RFS_PATH_SUBTREE))
			path_cmp = path_cmp->p_parent;
		
		else if (!list_empty(&path->p_rem))
			path_cmp = path_cmp->p_parent;
		
		else
			break;
	}

	if (path_cmp) {
		if (!chain_cmp(path_cmp->p_inchain, path->p_inchain_local) &&
		    !chain_cmp(path_cmp->p_exchain, path->p_exchain_local)) {

			chain_put(path->p_inchain_local);
			path->p_inchain_local = NULL;

			chain_put(path->p_exchain_local);
			path->p_exchain_local = NULL;

			path->p_flags &= ~RFS_PATH_SINGLE;

			if (!(path->p_flags & RFS_PATH_SUBTREE))
				list_add_tail(&path->p_rem, &path_rem_list);

			path_go = path_cmp;
		}
	}

	if (!inchain_local && (path->p_flags & RFS_PATH_SINGLE))
		return RFS_ERR_OK;

	retv = rfs_replace_ops(path, path_go, path_cmp);

	if (retv)
		return retv;

	return RFS_ERR_OK;
}

int flt_rem_local(struct path *path, void *data)
{
	struct chain *inchain_local = NULL;
	struct chain *exchain_local = NULL;
	struct path *path_go = path;
	struct path *path_cmp = path;
	int aux = 0;
	int *ops;
	int retv;

	while (path_cmp) {
		if (!(path_cmp->p_flags & RFS_PATH_SUBTREE))
			path_cmp = path_cmp->p_parent;
		
		else if (!list_empty(&path_cmp->p_rem))
			path_cmp = path_cmp->p_parent;
		
		else
			break;
	}

	if (path_cmp)
		aux = chain_find_flt(path_cmp->p_inchain, flt) != -1 || 
		      chain_find_flt(path_cmp->p_exchain, flt) != -1;

	if (chain_find_flt(path->p_inchain_local, flt) != -1 &&
	    chain_find_flt(path->p_exchain_local, flt) == -1) {

		inchain_local = chain_rem_flt(path->p_inchain_local, flt);
		if (IS_ERR(inchain_local))
			return PTR_ERR(inchain_local);

		chain_put(path->p_inchain_local);
		path->p_inchain_local = inchain_local;

		if (aux) {
			exchain_local = chain_add_flt(path->p_exchain_local, flt);
			if (IS_ERR(exchain_local))
				return PTR_ERR(exchain_local);
			
			chain_put(path->p_exchain_local);
			path->p_exchain_local = exchain_local;
		}
	}

	if (!aux && chain_find_flt(path->p_exchain_local, flt) != -1) {
		exchain = chain_rem_flt(path->p_exchain_local, flt);
		if (IS_ERR(exchain))
			return PTR_ERR(exchain);

		chain_put(path->p_exchain_local);
		path->p_exchain = exchain_local;
	}

	if (path_cmp) {
		if (!chain_cmp(path_cmp->p_inchain, path->p_inchain_local) &&
		    !chain_cmp(path_cmp->p_exchain, path->p_exchain_local)) {

			chain_put(path->p_inchain_local);
			path->p_inchain_local = NULL;

			chain_put(path->p_exchain_local);
			path->p_exchain_local = NULL;

			path_go = path_cmp;
		}
	}

	if (!path->p_inchain_local && !path->p_exchain_local) {

		path->p_flags &= ~RFS_PATH_SINGLE;

		if (!(path->p_flags & RFS_PATH_SUBTREE)) 
			list_add_tail(&path->p_rem, &path_rem_list);

		if (!path_cmp)
			remove = 1;
	}

	if (!inchain_local && (path->p_flags & RFS_PATH_SINGLE))
		return RFS_ERR_OK;

	if (!remove)
		retv = rfs_replace_ops(path, path_go);
	else 
		retv = rfs_restore_ops_cb(path->p_dentry, path)

	if (retv)
		return retv;

	return RFS_ERR_OK;
}

int flt_add_cb(struct path *path, void *data)
{
	struct filter *flt;
	struct chain *inchain = NULL;
	struct chain *exchain = NULL;
	struct path *path_go = path;
	struct path *path_cmp = path->p_parent;
	struct ops *ops;
	int retv;

	flt = (struct filter *)data;

	if (!(path->p_flags & RFS_PATH_SUBTREE))
		return flt_add_local(path, flt);

	if (chain_find_flt(path->p_inchain, flt) == -1) {
		inchain = chain_add_flt(path->p_inchain, flt);
		if (IS_ERR(inchain))
			return PTR_ERR(inchain);

		chain_put(path->p_inchain);
		path->p_inchain = inchain;

		if (chain_find_flt(path->p_exchain, flt) != -1) {
			exchain = chain_rem_flt(path->p_exchain, flt);
			if (IS_ERR(exchain))
				return PTR_ERR(exchain);

			chain_put(path->p_exchain);
			path->p_exchain = exchain;
		}
	}

	if (path->p_flags & RFS_PATH_SINGLE) {
		retv = flt_add_local(path, flt);
		if (retv)
			return retv;
	}

	while (path_cmp) {
		if (!(path_cmp->p_flags & RFS_PATH_SUBTREE))
			path_cmp = path_cmp->p_parent;
		
		else if (!list_empty(&path_cmp->p_rem))
			path_cmp = path_cmp->p_parent;
		
		else
			break;
	}

	if (path_cmp) {
		if (!chain_cmp(path_cmp->p_inchain, path->p_inchain) &&
		    !chain_cmp(path_cmp->p_exchain, path->p_exchain)) {

			chain_put(path->p_inchain);
			path->p_inchain = NULL;

			chain_put(path->p_exchain);
			path->p_exchain = NULL;

			ops_put(path->p_ops);
			path->p_flags &= ~RFS_PATH_SUBTREE;

			if (!(path->p_flags & RFS_PATH_SINGLE))
				list_add_tail(&path->p_rem, &path_rem_list);

			path_go = path_cmp;
		}
	}

	if (!inchain && (path->p_flags & RFS_PATH_SUBTREE))
		return RFS_ERR_OK;

	if (path->p_flags & RFS_PATH_SUBTREE) {
		ops = ops_alloc();
		if (IS_ERR(ops))
			return PTR_ERR(ops);

		chain_get_ops(path_go->p_inchain, ops->o_ops);
		ops_put(path_go->p_ops);
		path_go->p_ops = ops;
	}

	retv = rfs_walk_dcache(path->p_dentry, rfs_replace_ops_cb, path_go, NULL, NULL);

	if (retv)
		return retv;

	return RFS_ERR_OK;
}

int flt_rem_cb(struct path *path, void *data)
{
	struct filter *flt;
	struct chain *inchain = NULL;
	struct chain *exchain = NULL;
	struct path *path_go = path;
	struct path *path_cmp = path->p_parent;
	int remove = 0;
	int aux;
	int *ops;
	int retv;

	flt = (struct filter *)data;

	if (!(path->p_flags & RFS_PATH_SUBTREE))
		return flt_rem_local(path, flt);

	while (path_cmp) {
		if (!(path_cmp->p_flags & RFS_PATH_SUBTREE))
			path_cmp = path_cmp->p_parent;
		
		else if (!list_empty(&path_cmp->p_rem))
			path_cmp = path_cmp->p_parent;
		
		else
			break;
	}

	if (path_cmp)
		aux = chain_find_flt(path_cmp->p_inchain, flt) != -1 || 
		      chain_find_flt(path_cmp->p_exchain, flt) != -1;

	if (chain_find_flt(path->p_exchain, flt) == -1 &&
	    chain_find_flt(path->p_inchain, flt) != -1) {

		inchain = chain_rem_flt(path->p_inchain, flt);
		if (IS_ERR(inchain))
			return PTR_ERR(inchain);

		chain_put(path->p_inchain);
		path->p_inchain = inchain;

		if (aux) {
			exchain = chain_add_flt(path->p_exchain, flt);
			if (IS_ERR(exchain))
				return PTR_ERR(exchain);

			chain_put(path->p_exchain);
			path->p_exchain = exchain;
		}
	}
	
	if (!aux && chain_find_flt(path->p_exchain, flt) != -1) {
		exchain = chain_rem_flt(path->p_exchain, flt);
		if (IS_ERR(exchain))
			return PTR_ERR(exchain);

		chain_put(path->p_exchain);
		path->p_exchain = exchain;
	}

	if (path->p_flags & RFS_PATH_SINGLE) {
		retv = flt_rem_local(path, flt);
		if (retv)
			return retv;
	}

	if (path_cmp) {
		if (!chain_cmp(path_cmp->p_inchain, path->p_inchain) &&
		    !chain_cmp(path_cmp->p_exchain, path->p_exchain)) {

			chain_put(path->p_inchain);
			path->p_inchain = NULL;

			chain_put(path->p_exchain);
			path->p_exchain = NULL;

			path_go = path_cmp;
		}
	}

	if (!path->p_inchain && !path->p_exchain) {
		path->p_flags &= ~RFS_PATH_SUBTREE;
		ops_put(path->p_ops);

		if (!(path-p_flags & RFS_PATH_SINGLE))
			list_add_tail(&path->p_rem, &path_rem_list);

		if (!path_cmp)
			remove = 1;
	}

	if (!inchain && (path->p_flags & RFS_PATH_SUBTREE))
		return RFS_ERR_OK;

	if (!remove) {
		if (path->p_flags & RFS_PATH_SUBTREE) {
			ops = ops_alloc();
			if (IS_ERR(ops))
				return PTR_ERR(ops);

			chain_get_ops(path_go->p_inchain, ops);
			ops_put(path_go->p_ops);
			path_go->p_ops = ops;
		}

		retv = rfs_walk_dcache(path->p_dentry, rfs_replace_ops_cb, path_go, NULL, NULL);
	} else 
		retv = rfs_walk_dcache(path->p_dentry, rfs_restore_ops_cb, path, NULL, NULL);

	if (retv)
		return retv;

	return RFS_ERR_OK;
}

int flt_set_ops_cb(struct path *path, void *data)
{
	struct filter *flt;
	struct chain *chain;
	struct ops *ops;
	int err;

	flt = (struct filter *)data;

	if (chain_find_flt(path->p_inchain_local, flt) != -1) {
		err = rfs_set_ops(path->p_dentry, path);
		if (err)
			return err;
	}

	if (chain_find_flt(path->p_inchain, flt) != -1) {
		ops = ops_alloc();
		if (IS_ERR(ops))
			return PTR_ERR(ops);
		chain_get_ops(path->p_inchain, ops->o_ops);
		ops_put(path->p_ops);
		path->p_ops = ops;

		return rfs_walk_dcache(path->p_dentry, rfs_set_ops_cb, path, NULL, NULL);
	}

	return RFS_ERR_OK;
}

