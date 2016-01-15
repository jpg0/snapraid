/*
 * Copyright (C) 2011 Andrea Mazzoleni
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "portable.h"

#include "elem.h"
#include "support.h"
#include "util.h"

/****************************************************************************/
/* snapraid */

struct snapraid_content* content_alloc(const char* path, uint64_t dev)
{
	struct snapraid_content* content;

	content = malloc_nofail(sizeof(struct snapraid_content));
	pathimport(content->content, sizeof(content->content), path);
	content->device = dev;

	return content;
}

void content_free(struct snapraid_content* content)
{
	free(content);
}

struct snapraid_filter* filter_alloc_file(int direction, const char* pattern)
{
	struct snapraid_filter* filter;
	char* i;
	char* first;
	char* last;
	int token_is_valid;
	int token_is_filled;

	filter = malloc_nofail(sizeof(struct snapraid_filter));
	pathimport(filter->pattern, sizeof(filter->pattern), pattern);
	filter->direction = direction;

	/* find first and last slash */
	first = 0;
	last = 0;
	/* reject invalid tokens, like "<empty>", ".", ".." and more dots */
	token_is_valid = 0;
	token_is_filled = 0;
	for (i = filter->pattern; *i; ++i) {
		if (*i == '/') {
			/* reject invalid tokens, but accept an empty one as first */
			if (!token_is_valid && (first != 0 || token_is_filled)) {
				free(filter);
				return 0;
			}
			token_is_valid = 0;
			token_is_filled = 0;

			/* update slash position */
			if (!first)
				first = i;
			last = i;
		} else if (*i != '.') {
			token_is_valid = 1;
			token_is_filled = 1;
		} else {
			token_is_filled = 1;
		}
	}

	/* reject invalid tokens, but accept an empty one as last, but not if it's the only one */
	if (!token_is_valid && (first == 0 || token_is_filled)) {
		free(filter);
		return 0;
	}

	/* it's a file filter */
	filter->is_disk = 0;

	if (first == 0) {
		/* no slash */
		filter->is_path = 0;
		filter->is_dir = 0;
	} else if (first == last && last[1] == 0) {
		/* one slash at the end */
		filter->is_path = 0;
		filter->is_dir = 1;
		last[0] = 0;
	} else {
		/* at least a slash not at the end */
		filter->is_path = 1;
		if (last[1] == 0) {
			filter->is_dir = 1;
			last[0] = 0;
		} else {
			filter->is_dir = 0;
		}

		/* a slash must be the first char, as we don't support PATH/FILE and PATH/DIR/ */
		if (filter->pattern[0] != '/') {
			free(filter);
			return 0;
		}
	}

	return filter;
}

struct snapraid_filter* filter_alloc_disk(int direction, const char* pattern)
{
	struct snapraid_filter* filter;

	filter = malloc_nofail(sizeof(struct snapraid_filter));
	pathimport(filter->pattern, sizeof(filter->pattern), pattern);
	filter->direction = direction;

	/* it's a disk filter */
	filter->is_disk = 1;
	filter->is_path = 0;
	filter->is_dir = 0;

	/* no slash allowed in disk names */
	if (strchr(filter->pattern, '/') != 0) {
		/* LCOV_EXCL_START */
		free(filter);
		return 0;
		/* LCOV_EXCL_STOP */
	}

	return filter;
}

void filter_free(struct snapraid_filter* filter)
{
	free(filter);
}

const char* filter_type(struct snapraid_filter* filter, char* out, size_t out_size)
{
	const char* direction;
	if (filter->direction < 0)
		direction = "exclude";
	else
		direction = "include";

	if (filter->is_disk)
		pathprint(out, out_size, "%s %s:", direction, filter->pattern);
	else if (filter->is_dir)
		pathprint(out, out_size, "%s %s/", direction, filter->pattern);
	else
		pathprint(out, out_size, "%s %s", direction, filter->pattern);

	return out;
}

static int filter_apply(struct snapraid_filter* filter, struct snapraid_filter** reason, const char* path, const char* name, int is_dir)
{
	int ret = 0;

	/* match dirs with dirs and files with files */
	if (filter->is_dir && !is_dir)
		return 0;
	if (!filter->is_dir && is_dir)
		return 0;

	if (filter->is_path) {
		/* skip initial slash, as always missing from the path */
		if (fnmatch(filter->pattern + 1, path, FNM_PATHNAME | FNM_CASEINSENSITIVE_FOR_WIN) == 0)
			ret = filter->direction;
	} else {
		if (fnmatch(filter->pattern, name, FNM_CASEINSENSITIVE_FOR_WIN) == 0)
			ret = filter->direction;
	}

	if (reason != 0 && ret < 0)
		*reason = filter;

	return ret;
}

static int filter_recurse(struct snapraid_filter* filter, struct snapraid_filter** reason, const char* const_path, int is_dir)
{
	char path[PATH_MAX];
	char* name;
	unsigned i;

	pathcpy(path, sizeof(path), const_path);

	/* filter for all the directories */
	name = path;
	for (i = 0; path[i] != 0; ++i) {
		if (path[i] == '/') {
			/* set a terminator */
			path[i] = 0;

			/* filter the directory */
			if (filter_apply(filter, reason, path, name, 1) != 0)
				return filter->direction;

			/* restore the slash */
			path[i] = '/';

			/* next name */
			name = path + i + 1;
		}
	}

	/* filter the final file */
	if (filter_apply(filter, reason, path, name, is_dir) != 0)
		return filter->direction;

	return 0;
}

static int filter_element(tommy_list* filterlist, struct snapraid_filter** reason, const char* disk, const char* sub, int is_dir)
{
	tommy_node* i;

	int direction = 1; /* by default include all */

	/* for each filter */
	for (i = tommy_list_head(filterlist); i != 0; i = i->next) {
		int ret;
		struct snapraid_filter* filter = i->data;

		if (filter->is_disk) {
			if (fnmatch(filter->pattern, disk, FNM_CASEINSENSITIVE_FOR_WIN) == 0)
				ret = filter->direction;
			else
				ret = 0;
			if (reason != 0 && ret < 0)
				*reason = filter;
		} else {
			ret = filter_recurse(filter, reason, sub, is_dir);
		}

		if (ret > 0) {
			/* include the file */
			return 0;
		} else if (ret < 0) {
			/* exclude the file */
			return -1;
		} else {
			/* default is opposite of the last filter */
			direction = -filter->direction;
			if (reason != 0 && direction < 0)
				*reason = filter;
			/* continue with the next one */
		}
	}

	/* directories are always included by default, otherwise we cannot apply rules */
	/* to the contained files */
	if (is_dir)
		return 0;

	/* files are excluded/included depending of the last rule processed */
	if (direction < 0)
		return -1;

	return 0;
}

int filter_path(tommy_list* filterlist, struct snapraid_filter** reason, const char* disk, const char* sub)
{
	return filter_element(filterlist, reason, disk, sub, 0);
}

int filter_dir(tommy_list* filterlist, struct snapraid_filter** reason, const char* disk, const char* sub)
{
	return filter_element(filterlist, reason, disk, sub, 1);
}

int filter_existence(int filter_missing, const char* dir, const char* sub)
{
	char path[PATH_MAX];
	struct stat st;

	if (!filter_missing)
		return 0;

	/* we directly check if in the disk the file is present or not */
	pathprint(path, sizeof(path), "%s%s", dir, sub);

	if (lstat(path, &st) != 0) {
		/* if the file doesn't exist, we don't filter it out */
		if (errno == ENOENT)
			return 0;
		/* LCOV_EXCL_START */
		log_fatal("Error in stat file '%s'. %s.\n", path, strerror(errno));
		exit(EXIT_FAILURE);
		/* LCOV_EXCL_STOP */
	}

	/* the file is present, so we filter it out */
	return 1;
}

int filter_correctness(int filter_error, tommy_arrayblkof* infoarr, struct snapraid_disk* disk, struct snapraid_file* file)
{
	unsigned i;

	if (!filter_error)
		return 0;

	/* check each block of the file */
	for (i = 0; i < file->blockmax; ++i) {
		block_off_t parity_pos = fs_file2par_get(disk, file, i);
		snapraid_info info = info_get(infoarr, parity_pos);

		/* if the file has a bad block, don't exclude it */
		if (info_get_bad(info))
			return 0;
	}

	/* the file is correct, so we filter it out */
	return 1;
}

int filter_content(tommy_list* contentlist, const char* path)
{
	tommy_node* i;

	for (i = tommy_list_head(contentlist); i != 0; i = i->next) {
		struct snapraid_content* content = i->data;
		char tmp[PATH_MAX];

		if (pathcmp(content->content, path) == 0)
			return -1;

		/* exclude also the ".tmp" copy used to save it */
		pathprint(tmp, sizeof(tmp), "%s.tmp", content->content);
		if (pathcmp(tmp, path) == 0)
			return -1;

		/* exclude also the ".lock" file */
		pathprint(tmp, sizeof(tmp), "%s.lock", content->content);
		if (pathcmp(tmp, path) == 0)
			return -1;
	}

	return 0;
}

struct snapraid_file* file_alloc(unsigned block_size, const char* sub, data_off_t size, uint64_t mtime_sec, int mtime_nsec, uint64_t inode, uint64_t physical)
{
	struct snapraid_file* file;
	block_off_t i;

	file = malloc_nofail(sizeof(struct snapraid_file));
	file->sub = strdup_nofail(sub);
	file->size = size;
	file->blockmax = (size + block_size - 1) / block_size;
	file->mtime_sec = mtime_sec;
	file->mtime_nsec = mtime_nsec;
	file->inode = inode;
	file->physical = physical;
	file->flag = 0;
	file->blockvec = malloc_nofail(file->blockmax * sizeof(struct snapraid_block));

	for (i = 0; i < file->blockmax; ++i) {
		block_state_set(&file->blockvec[i], BLOCK_STATE_CHG);
		hash_invalid_set(file->blockvec[i].hash);
	}

	return file;
}

struct snapraid_file* file_dup(struct snapraid_file* copy)
{
	struct snapraid_file* file;
	block_off_t i;

	file = malloc_nofail(sizeof(struct snapraid_file));
	file->sub = strdup_nofail(copy->sub);
	file->size = copy->size;
	file->blockmax = copy->blockmax;
	file->mtime_sec = copy->mtime_sec;
	file->mtime_nsec = copy->mtime_nsec;
	file->inode = copy->inode;
	file->physical = copy->physical;
	file->flag = copy->flag;
	file->blockvec = malloc_nofail(file->blockmax * sizeof(struct snapraid_block));

	for (i = 0; i < file->blockmax; ++i) {
		file->blockvec[i].state = copy->blockvec[i].state;
		memcpy(file->blockvec[i].hash, copy->blockvec[i].hash, HASH_SIZE);
	}

	return file;
}

void file_free(struct snapraid_file* file)
{
	free(file->sub);
	file->sub = 0;
	free(file->blockvec);
	file->blockvec = 0;
	free(file);
}

void file_rename(struct snapraid_file* file, const char* sub)
{
	free(file->sub);
	file->sub = strdup_nofail(sub);
}

void file_copy(struct snapraid_file* src_file, struct snapraid_file* dst_file)
{
	block_off_t i;

	if (src_file->size != dst_file->size) {
		/* LCOV_EXCL_START */
		log_fatal("Internal inconsistency in copy file with different size\n");
		os_abort();
		/* LCOV_EXCL_STOP */
	}

	if (src_file->mtime_sec != dst_file->mtime_sec) {
		/* LCOV_EXCL_START */
		log_fatal("Internal inconsistency in copy file with different mtime_sec\n");
		os_abort();
		/* LCOV_EXCL_STOP */
	}

	if (src_file->mtime_nsec != dst_file->mtime_nsec) {
		/* LCOV_EXCL_START */
		log_fatal("Internal inconsistency in copy file with different mtime_nsec\n");
		os_abort();
		/* LCOV_EXCL_STOP */
	}

	for (i = 0; i < dst_file->blockmax; ++i) {
		/* set a block with hash computed but without parity */
		block_state_set(&dst_file->blockvec[i], BLOCK_STATE_REP);

		/* copy the hash */
		memcpy(dst_file->blockvec[i].hash, src_file->blockvec[i].hash, HASH_SIZE);
	}

	file_flag_set(dst_file, FILE_IS_COPY);
}

const char* file_name(const struct snapraid_file* file)
{
	const char* r = strrchr(file->sub, '/');

	if (!r)
		r = file->sub;
	else
		++r;
	return r;
}

unsigned file_block_size(struct snapraid_file* file, block_off_t file_pos, unsigned block_size)
{
	/* if it's the last block */
	if (file_pos + 1 == file->blockmax) {
		unsigned block_remainder;
		if (file->size == 0)
			return 0;
		block_remainder = file->size % block_size;
		if (block_remainder == 0)
			block_remainder = block_size;
		return block_remainder;
	}

	return block_size;
}

int file_block_is_last(struct snapraid_file* file, block_off_t file_pos)
{
	if (file_pos == 0 && file->blockmax == 0)
		return 1;

	if (file_pos >= file->blockmax) {
		/* LCOV_EXCL_START */
		log_fatal("Internal inconsistency in file block position\n");
		os_abort();
		/* LCOV_EXCL_STOP */
	}

	return file_pos == file->blockmax - 1;
}

int file_inode_compare_to_arg(const void* void_arg, const void* void_data)
{
	const uint64_t* arg = void_arg;
	const struct snapraid_file* file = void_data;

	if (*arg < file->inode)
		return -1;
	if (*arg > file->inode)
		return 1;
	return 0;
}

int file_inode_compare(const void* void_a, const void* void_b)
{
	const struct snapraid_file* file_a = void_a;
	const struct snapraid_file* file_b = void_b;

	if (file_a->inode < file_b->inode)
		return -1;
	if (file_a->inode > file_b->inode)
		return 1;
	return 0;
}

int file_path_compare(const void* void_a, const void* void_b)
{
	const struct snapraid_file* file_a = void_a;
	const struct snapraid_file* file_b = void_b;

	return strcmp(file_a->sub, file_b->sub);
}

int file_physical_compare(const void* void_a, const void* void_b)
{
	const struct snapraid_file* file_a = void_a;
	const struct snapraid_file* file_b = void_b;

	if (file_a->physical < file_b->physical)
		return -1;
	if (file_a->physical > file_b->physical)
		return 1;
	return 0;
}

int file_path_compare_to_arg(const void* void_arg, const void* void_data)
{
	const char* arg = void_arg;
	const struct snapraid_file* file = void_data;

	return strcmp(arg, file->sub);
}

int file_name_compare(const void* void_a, const void* void_b)
{
	const struct snapraid_file* file_a = void_a;
	const struct snapraid_file* file_b = void_b;
	const char* name_a = file_name(file_a);
	const char* name_b = file_name(file_b);

	return strcmp(name_a, name_b);
}

int file_stamp_compare(const void* void_a, const void* void_b)
{
	const struct snapraid_file* file_a = void_a;
	const struct snapraid_file* file_b = void_b;

	if (file_a->size < file_b->size)
		return -1;
	if (file_a->size > file_b->size)
		return 1;

	if (file_a->mtime_sec < file_b->mtime_sec)
		return -1;
	if (file_a->mtime_sec > file_b->mtime_sec)
		return 1;

	if (file_a->mtime_nsec < file_b->mtime_nsec)
		return -1;
	if (file_a->mtime_nsec > file_b->mtime_nsec)
		return 1;

	return 0;
}

int file_namestamp_compare(const void* void_a, const void* void_b)
{
	int ret;

	ret = file_name_compare(void_a, void_b);
	if (ret != 0)
		return ret;

	return file_stamp_compare(void_a, void_b);
}

int file_pathstamp_compare(const void* void_a, const void* void_b)
{
	int ret;

	ret = file_path_compare(void_a, void_b);
	if (ret != 0)
		return ret;

	return file_stamp_compare(void_a, void_b);
}

struct snapraid_chunk* chunk_alloc(block_off_t parity_pos, struct snapraid_file* file, block_off_t file_pos, block_off_t count)
{
	struct snapraid_chunk* chunk;

	if (count == 0) {
		/* LCOV_EXCL_START */
		log_fatal("Internal inconsistency when allocating empty chunk for file '%s' at position '%u/%u'\n", file->sub, file_pos, file->blockmax);
		os_abort();
		/* LCOV_EXCL_STOP */
	}
	if (file_pos + count > file->blockmax) {
		/* LCOV_EXCL_START */
		log_fatal("Internal inconsistency when allocating overflowing chunk for file '%s' at position '%u:%u/%u'\n", file->sub, file_pos, count, file->blockmax);
		os_abort();
		/* LCOV_EXCL_STOP */
	}

	chunk = malloc_nofail(sizeof(struct snapraid_chunk));
	chunk->parity_pos = parity_pos;
	chunk->file = file;
	chunk->file_pos = file_pos;
	chunk->count = count;

	return chunk;
}

void chunk_free(struct snapraid_chunk* chunk)
{
	free(chunk);
}

int chunk_parity_compare(const void* void_a, const void* void_b)
{
	const struct snapraid_chunk* arg_a = void_a;
	const struct snapraid_chunk* arg_b = void_b;

	if (arg_a->parity_pos < arg_b->parity_pos)
		return -1;
	if (arg_a->parity_pos > arg_b->parity_pos)
		return 1;

	return 0;
}

int chunk_file_compare(const void* void_a, const void* void_b)
{
	const struct snapraid_chunk* arg_a = void_a;
	const struct snapraid_chunk* arg_b = void_b;

	if (arg_a->file < arg_b->file)
		return -1;
	if (arg_a->file > arg_b->file)
		return 1;

	if (arg_a->file_pos < arg_b->file_pos)
		return -1;
	if (arg_a->file_pos > arg_b->file_pos)
		return 1;

	return 0;
}

struct snapraid_link* link_alloc(const char* sub, const char* linkto, unsigned link_flag)
{
	struct snapraid_link* slink;

	slink = malloc_nofail(sizeof(struct snapraid_link));
	slink->sub = strdup_nofail(sub);
	slink->linkto = strdup_nofail(linkto);
	slink->flag = link_flag;

	return slink;
}

void link_free(struct snapraid_link* slink)
{
	free(slink->sub);
	free(slink->linkto);
	free(slink);
}

int link_name_compare_to_arg(const void* void_arg, const void* void_data)
{
	const char* arg = void_arg;
	const struct snapraid_link* slink = void_data;

	return strcmp(arg, slink->sub);
}

int link_alpha_compare(const void* void_a, const void* void_b)
{
	const struct snapraid_link* slink_a = void_a;
	const struct snapraid_link* slink_b = void_b;

	return strcmp(slink_a->sub, slink_b->sub);
}

struct snapraid_dir* dir_alloc(const char* sub)
{
	struct snapraid_dir* dir;

	dir = malloc_nofail(sizeof(struct snapraid_dir));
	dir->sub = strdup_nofail(sub);
	dir->flag = 0;

	return dir;
}

void dir_free(struct snapraid_dir* dir)
{
	free(dir->sub);
	free(dir);
}

int dir_name_compare(const void* void_arg, const void* void_data)
{
	const char* arg = void_arg;
	const struct snapraid_dir* dir = void_data;

	return strcmp(arg, dir->sub);
}

struct snapraid_disk* disk_alloc(const char* name, const char* dir, uint64_t dev, const char* uuid, int skip)
{
	struct snapraid_disk* disk;

	disk = malloc_nofail(sizeof(struct snapraid_disk));
	pathcpy(disk->name, sizeof(disk->name), name);
	pathimport(disk->dir, sizeof(disk->dir), dir);
	pathcpy(disk->uuid, sizeof(disk->uuid), uuid);

	/* ensure that the dir terminate with "/" if it isn't empty */
	pathslash(disk->dir, sizeof(disk->dir));

	pthread_mutex_init(&disk->fs_mutex, 0);

	disk->smartctl[0] = 0;
	disk->device = dev;
	disk->tick = 0;
	disk->total_blocks = 0;
	disk->free_blocks = 0;
	disk->first_free_block = 0;
	disk->has_volatile_inodes = 0;
	disk->has_unreliable_physical = 0;
	disk->has_different_uuid = 0;
	disk->has_unsupported_uuid = *uuid == 0; /* empty UUID means unsupported */
	disk->had_empty_uuid = 0;
	disk->mapping_idx = -1;
	disk->skip_access = skip;
	tommy_list_init(&disk->filelist);
	tommy_list_init(&disk->deletedlist);
	tommy_hashdyn_init(&disk->inodeset);
	tommy_hashdyn_init(&disk->pathset);
	tommy_hashdyn_init(&disk->stampset);
	tommy_list_init(&disk->linklist);
	tommy_hashdyn_init(&disk->linkset);
	tommy_list_init(&disk->dirlist);
	tommy_hashdyn_init(&disk->dirset);
	tommy_tree_init(&disk->fs_parity, chunk_parity_compare);
	tommy_tree_init(&disk->fs_file, chunk_file_compare);
	disk->fs_last = 0;

	return disk;
}

void disk_free(struct snapraid_disk* disk)
{
	tommy_list_foreach(&disk->filelist, (tommy_foreach_func*)file_free);
	tommy_list_foreach(&disk->deletedlist, (tommy_foreach_func*)file_free);
	tommy_tree_foreach(&disk->fs_file, (tommy_foreach_func*)chunk_free);
	tommy_hashdyn_done(&disk->inodeset);
	tommy_hashdyn_done(&disk->pathset);
	tommy_hashdyn_done(&disk->stampset);
	tommy_list_foreach(&disk->linklist, (tommy_foreach_func*)link_free);
	tommy_hashdyn_done(&disk->linkset);
	tommy_list_foreach(&disk->dirlist, (tommy_foreach_func*)dir_free);
	tommy_hashdyn_done(&disk->dirset);

	pthread_mutex_destroy(&disk->fs_mutex);

	free(disk);
}

static inline void fs_lock(struct snapraid_disk* disk)
{
	pthread_mutex_lock(&disk->fs_mutex);
}

static inline void fs_unlock(struct snapraid_disk* disk)
{
	pthread_mutex_unlock(&disk->fs_mutex);
}

struct chunk_disk_empty {
	block_off_t blockmax;
};

/**
 * Compare the chunk if inside the specified blockmax.
 */
static int chunk_disk_empty_compare_unlock(const void* void_a, const void* void_b)
{
	const struct chunk_disk_empty* arg_a = void_a;
	const struct snapraid_chunk* arg_b = void_b;

	/* if the block is inside the specified blockmax, it's found */
	if (arg_a->blockmax > arg_b->parity_pos)
		return 0;

	/* otherwise search for a smaller one */
	return -1;
}

int fs_is_empty(struct snapraid_disk* disk, block_off_t blockmax)
{
	struct chunk_disk_empty arg = { blockmax };

	/* if there is an element, it's not empty */
	/* even if links and dirs have no block allocation */
	if (!tommy_list_empty(&disk->filelist))
		return 0;
	if (!tommy_list_empty(&disk->linklist))
		return 0;
	if (!tommy_list_empty(&disk->dirlist))
		return 0;

	fs_lock(disk);

	/* search for any chunk inside blockmax */
	if (tommy_tree_search_compare(&disk->fs_parity, chunk_disk_empty_compare_unlock, &arg) != 0) {
		fs_unlock(disk);
		return 0;
	}

	/* finally, it's empty */
	fs_unlock(disk);
	return 1;
}

struct chunk_disk_size {
	block_off_t size;
};

/**
 * Compare the chunk by highest parity position.
 *
 * The maximum parity position is stored as size.
 */
static int chunk_disk_size_compare_unlock(const void* void_a, const void* void_b)
{
	struct chunk_disk_size* arg_a = (void*)void_a;
	const struct snapraid_chunk* arg_b = void_b;

	/* get the maximum size */
	if (arg_a->size < arg_b->parity_pos + arg_b->count)
		arg_a->size = arg_b->parity_pos + arg_b->count;

	/* search always for a bigger one */
	return 1;
}

block_off_t fs_size(struct snapraid_disk* disk)
{
	struct chunk_disk_size arg = { 0 };

	fs_lock(disk);

	tommy_tree_search_compare(&disk->fs_parity, chunk_disk_size_compare_unlock, &arg);

	fs_unlock(disk);

	return arg.size;
}

struct chunk_check {
	const struct snapraid_chunk* prev;
	int result;
};

static void chunk_parity_check_foreach_unlock(void* void_arg, void* void_obj)
{
	struct chunk_check* arg = void_arg;
	const struct snapraid_chunk* obj = void_obj;
	const struct snapraid_chunk* prev = arg->prev;

	/* set the next previous block */
	arg->prev = obj;

	/* stop reporting if too many errors */
	if (arg->result > 100) {
		/* LCOV_EXCL_START */
		return;
		/* LCOV_EXCL_STOP */
	}

	if (obj->count == 0) {
		/* LCOV_EXCL_START */
		log_fatal("Internal inconsistency in parity count zero for file '%s' at '%u'\n",
			obj->file->sub, obj->parity_pos);
		++arg->result;
		return;
		/* LCOV_EXCL_STOP */
	}

	/* check only if there is a previous block */
	if (!prev)
		return;

	/* check the order */
	if (prev->parity_pos >= obj->parity_pos) {
		/* LCOV_EXCL_START */
		log_fatal("Internal inconsistency in parity order for files '%s' at '%u:%u' and '%s' at '%u:%u'\n",
			prev->file->sub, prev->parity_pos, prev->count, obj->file->sub, obj->parity_pos, obj->count);
		++arg->result;
		return;
		/* LCOV_EXCL_STOP */
	}

	/* check that the chunks don't overlap */
	if (prev->parity_pos + prev->count > obj->parity_pos) {
		/* LCOV_EXCL_START */
		log_fatal("Internal inconsistency for parity overlap for files '%s' at '%u:%u' and '%s' at '%u:%u'\n",
			prev->file->sub, prev->parity_pos, prev->count, obj->file->sub, obj->parity_pos, obj->count);
		++arg->result;
		return;
		/* LCOV_EXCL_STOP */
	}
}

static void chunk_file_check_foreach_unlock(void* void_arg, void* void_obj)
{
	struct chunk_check* arg = void_arg;
	const struct snapraid_chunk* obj = void_obj;
	const struct snapraid_chunk* prev = arg->prev;

	/* set the next previous block */
	arg->prev = obj;

	/* stop reporting if too many errors */
	if (arg->result > 100) {
		/* LCOV_EXCL_START */
		return;
		/* LCOV_EXCL_STOP */
	}

	if (obj->count == 0) {
		/* LCOV_EXCL_START */
		log_fatal("Internal inconsistency in file count zero for file '%s' at '%u'\n",
			obj->file->sub, obj->file_pos);
		++arg->result;
		return;
		/* LCOV_EXCL_STOP */
	}

	/* note that for deleted files, some chunks may be missing */

	/* if the files are different */
	if (!prev || prev->file != obj->file) {
		if (prev != 0) {
			if (file_flag_has(prev->file, FILE_IS_DELETED)) {
				/* check that the chunk doesn't overflow the file */
				if (prev->file_pos + prev->count > prev->file->blockmax) {
					/* LCOV_EXCL_START */
					log_fatal("Internal inconsistency in delete end for file '%s' at '%u:%u' overflowing size '%u'\n",
						prev->file->sub, prev->file_pos, prev->count, prev->file->blockmax);
					++arg->result;
					return;
					/* LCOV_EXCL_STOP */
				}
			} else {
				/* check that the chunk ends the file */
				if (prev->file_pos + prev->count != prev->file->blockmax) {
					/* LCOV_EXCL_START */
					log_fatal("Internal inconsistency in file end for file '%s' at '%u:%u' instead of size '%u'\n",
						prev->file->sub, prev->file_pos, prev->count, prev->file->blockmax);
					++arg->result;
					return;
					/* LCOV_EXCL_STOP */
				}
			}
		}

		if (file_flag_has(obj->file, FILE_IS_DELETED)) {
			/* check that the chunk doesn't overflow the file */
			if (obj->file_pos + obj->count > obj->file->blockmax) {
				/* LCOV_EXCL_START */
				log_fatal("Internal inconsistency in delete start for file '%s' at '%u:%u' overflowing size '%u'\n",
					obj->file->sub, obj->file_pos, obj->count, obj->file->blockmax);
				++arg->result;
				return;
				/* LCOV_EXCL_STOP */
			}
		} else {
			/* check that the chunk starts the file */
			if (obj->file_pos != 0) {
				/* LCOV_EXCL_START */
				log_fatal("Internal inconsistency in file start for file '%s' at '%u:%u'\n",
					obj->file->sub, obj->file_pos, obj->count);
				++arg->result;
				return;
				/* LCOV_EXCL_STOP */
			}
		}
	} else {
		/* check the order */
		if (prev->file_pos >= obj->file_pos) {
			/* LCOV_EXCL_START */
			log_fatal("Internal inconsistency in file order for file '%s' at '%u:%u' and at '%u:%u'\n",
				prev->file->sub, prev->file_pos, prev->count, obj->file_pos, obj->count);
			++arg->result;
			return;
			/* LCOV_EXCL_STOP */
		}

		if (file_flag_has(obj->file, FILE_IS_DELETED)) {
			/* check that the chunks don't overlap */
			if (prev->file_pos + prev->count > obj->file_pos) {
				/* LCOV_EXCL_START */
				log_fatal("Internal inconsistency in delete sequence for file '%s' at '%u:%u' and at '%u:%u'\n",
					prev->file->sub, prev->file_pos, prev->count, obj->file_pos, obj->count);
				++arg->result;
				return;
				/* LCOV_EXCL_STOP */
			}
		} else {
			/* check that the chunks are sequential */
			if (prev->file_pos + prev->count != obj->file_pos) {
				/* LCOV_EXCL_START */
				log_fatal("Internal inconsistency in file sequence for file '%s' at '%u:%u' and at '%u:%u'\n",
					prev->file->sub, prev->file_pos, prev->count, obj->file_pos, obj->count);
				++arg->result;
				return;
				/* LCOV_EXCL_STOP */
			}
		}
	}
}

int fs_check(struct snapraid_disk* disk)
{
	struct chunk_check arg;

	/* error count starts from 0 */
	arg.result = 0;

	fs_lock(disk);

	/* check parity sequence */
	arg.prev = 0;
	tommy_tree_foreach_arg(&disk->fs_parity, chunk_parity_check_foreach_unlock, &arg);

	/* check file sequence */
	arg.prev = 0;
	tommy_tree_foreach_arg(&disk->fs_file, chunk_file_check_foreach_unlock, &arg);

	fs_unlock(disk);

	if (arg.result != 0)
		return -1;

	return 0;
}

struct chunk_parity_inside {
	block_off_t parity_pos;
};

/**
 * Compare the chunk if containing the specified parity position.
 */
static int chunk_parity_inside_compare_unlock(const void* void_a, const void* void_b)
{
	const struct chunk_parity_inside* arg_a = void_a;
	const struct snapraid_chunk* arg_b = void_b;

	if (arg_a->parity_pos < arg_b->parity_pos)
		return -1;
	if (arg_a->parity_pos >= arg_b->parity_pos + arg_b->count)
		return 1;

	return 0;
}

/**
 * Seach the chunk at the specified parity position.
 * The search is optimized for sequential accesses.
 * \return If not found return 0
 */
static struct snapraid_chunk* fs_par2chunk_get_unlock(struct snapraid_disk* disk, struct snapraid_chunk** fs_last, block_off_t parity_pos)
{
	struct snapraid_chunk* chunk;

	/* check if the last accessed chunk matches */
	if (*fs_last
		&& parity_pos >= (*fs_last)->parity_pos
		&& parity_pos < (*fs_last)->parity_pos + (*fs_last)->count
	) {
		chunk = *fs_last;
	} else {
		struct chunk_parity_inside arg = { parity_pos };
		chunk = tommy_tree_search_compare(&disk->fs_parity, chunk_parity_inside_compare_unlock, &arg);
	}

	if (!chunk)
		return 0;

	/* store the last accessed chunk */
	*fs_last = chunk;

	return chunk;
}

struct chunk_file_inside {
	struct snapraid_file* file;
	block_off_t file_pos;
};

/**
 * Compare the chunk if containing the specified file position.
 */
static int chunk_file_inside_compare_unlock(const void* void_a, const void* void_b)
{
	const struct chunk_file_inside* arg_a = void_a;
	const struct snapraid_chunk* arg_b = void_b;

	if (arg_a->file < arg_b->file)
		return -1;
	if (arg_a->file > arg_b->file)
		return 1;

	if (arg_a->file_pos < arg_b->file_pos)
		return -1;
	if (arg_a->file_pos >= arg_b->file_pos + arg_b->count)
		return 1;

	return 0;
}

/**
 * Search the chunk at the specified file position.
 * The search is optimized for sequential accesses.
 * \return If not found return 0
 */
static struct snapraid_chunk* fs_file2chunk_get_unlock(struct snapraid_disk* disk, struct snapraid_chunk** fs_last, struct snapraid_file* file, block_off_t file_pos)
{
	struct snapraid_chunk* chunk;

	/* check if the last accessed chunk matches */
	if (*fs_last
		&& file == (*fs_last)->file
		&& file_pos >= (*fs_last)->file_pos
		&& file_pos < (*fs_last)->file_pos + (*fs_last)->count
	) {
		chunk = *fs_last;
	} else {
		struct chunk_file_inside arg = { file, file_pos };
		chunk = tommy_tree_search_compare(&disk->fs_file, chunk_file_inside_compare_unlock, &arg);
	}

	if (!chunk)
		return 0;

	/* store the last accessed chunk */
	*fs_last = chunk;

	return chunk;
}

struct snapraid_file* fs_par2file_get(struct snapraid_disk* disk, block_off_t parity_pos, block_off_t* file_pos)
{
	struct snapraid_chunk* chunk;
	struct snapraid_file* file;

	fs_lock(disk);

	chunk = fs_par2chunk_get_unlock(disk, &disk->fs_last, parity_pos);

	if (!chunk) {
		fs_unlock(disk);
		return 0;
	}

	if (file_pos)
		*file_pos = chunk->file_pos + (parity_pos - chunk->parity_pos);

	file = chunk->file;

	fs_unlock(disk);
	return file;
}

block_off_t fs_file2par_maybe(struct snapraid_disk* disk, struct snapraid_file* file, block_off_t file_pos)
{
	struct snapraid_chunk* chunk;
	block_off_t ret;

	fs_lock(disk);

	chunk = fs_file2chunk_get_unlock(disk, &disk->fs_last, file, file_pos);
	if (!chunk) {
		fs_unlock(disk);
		return POS_INVALID;
	}

	ret = chunk->parity_pos + (file_pos - chunk->file_pos);

	fs_unlock(disk);
	return ret;
}

void fs_allocate(struct snapraid_disk* disk, block_off_t parity_pos, struct snapraid_file* file, block_off_t file_pos)
{
	struct snapraid_chunk* chunk;
	struct snapraid_chunk* parity_chunk;
	struct snapraid_chunk* file_chunk;

	fs_lock(disk);

	if (file_pos > 0) {
		/* search an existing chunk for the previous file_pos */
		chunk = fs_file2chunk_get_unlock(disk, &disk->fs_last, file, file_pos - 1);

		if (chunk != 0 && parity_pos == chunk->parity_pos + chunk->count) {
			/* ensure that we are extending the chunk at the end */
			if (file_pos != chunk->file_pos + chunk->count) {
				/* LCOV_EXCL_START */
				log_fatal("Internal inconsistency when allocating file '%s' at position '%u/%u' in the middle of chunk '%u:%u' in disk '%s'\n", file->sub, file_pos, file->blockmax, chunk->file_pos, chunk->count, disk->name);
				os_abort();
				/* LCOV_EXCL_STOP */
			}

			/* extend the existing chunk */
			++chunk->count;

			fs_unlock(disk);
			return;
		}
	}

	/* a chunk doesn't exist, and we have to create a new one */
	chunk = chunk_alloc(parity_pos, file, file_pos, 1);

	/* insert the chunk in the trees */
	parity_chunk = tommy_tree_insert(&disk->fs_parity, &chunk->parity_node, chunk);
	file_chunk = tommy_tree_insert(&disk->fs_file, &chunk->file_node, chunk);

	if (parity_chunk != chunk || file_chunk != chunk) {
		/* LCOV_EXCL_START */
		log_fatal("Internal inconsistency when allocating file '%s' at position '%u/%u' for existing chunk '%u:%u' in disk '%s'\n", file->sub, file_pos, file->blockmax, chunk->file_pos, chunk->count, disk->name);
		os_abort();
		/* LCOV_EXCL_STOP */
	}

	/* store the last accessed chunk */
	disk->fs_last = chunk;

	fs_unlock(disk);
}

void fs_deallocate(struct snapraid_disk* disk, block_off_t parity_pos)
{
	struct snapraid_chunk* chunk;
	struct snapraid_chunk* second_chunk;
	struct snapraid_chunk* parity_chunk;
	struct snapraid_chunk* file_chunk;
	block_off_t first_count, second_count;

	fs_lock(disk);

	chunk = fs_par2chunk_get_unlock(disk, &disk->fs_last, parity_pos);
	if (!chunk) {
		/* LCOV_EXCL_START */
		log_fatal("Internal inconsistency when deallocating parity position '%u' for not existing chunk in disk '%s'\n", parity_pos, disk->name);
		os_abort();
		/* LCOV_EXCL_STOP */
	}

	/* if it's the only block of the chunk, delete it */
	if (chunk->count == 1) {
		/* remove from the trees */
		tommy_tree_remove(&disk->fs_parity, chunk);
		tommy_tree_remove(&disk->fs_file, chunk);

		/* deallocate */
		chunk_free(chunk);

		/* clear the last accessed chunk */
		disk->fs_last = 0;

		fs_unlock(disk);
		return;
	}

	/* if it's at the start of the chunk, shrink the chunk */
	if (parity_pos == chunk->parity_pos) {
		++chunk->parity_pos;
		++chunk->file_pos;
		--chunk->count;

		fs_unlock(disk);
		return;
	}

	/* if it's at the end of the chunk, shrink the chunk */
	if (parity_pos == chunk->parity_pos + chunk->count - 1) {
		--chunk->count;

		fs_unlock(disk);
		return;
	}

	/* otherwise it's in the middle */
	first_count = parity_pos - chunk->parity_pos;
	second_count = chunk->count - first_count - 1;

	/* adjust the first chunk */
	chunk->count = first_count;

	/* allocate the second chunk */
	second_chunk = chunk_alloc(chunk->parity_pos + first_count + 1, chunk->file, chunk->file_pos + first_count + 1, second_count);

	/* insert the chunk in the trees */
	parity_chunk = tommy_tree_insert(&disk->fs_parity, &second_chunk->parity_node, second_chunk);
	file_chunk = tommy_tree_insert(&disk->fs_file, &second_chunk->file_node, second_chunk);

	if (parity_chunk != second_chunk || file_chunk != second_chunk) {
		/* LCOV_EXCL_START */
		log_fatal("Internal inconsistency when deallocating parity position '%u' for splitting chunk '%u:%u' in disk '%s'\n", parity_pos, second_chunk->file_pos, second_chunk->count, disk->name);
		os_abort();
		/* LCOV_EXCL_STOP */
	}

	/* store the last accessed chunk */
	disk->fs_last = second_chunk;

	fs_unlock(disk);
}

struct snapraid_block* fs_file2block_get(struct snapraid_file* file, block_off_t file_pos)
{
	if (file_pos >= file->blockmax) {
		/* LCOV_EXCL_START */
		log_fatal("Internal inconsistency when dereferencing file '%s' at position '%u/%u'\n", file->sub, file_pos, file->blockmax);
		os_abort();
		/* LCOV_EXCL_STOP */
	}

	return &file->blockvec[file_pos];
}

struct snapraid_block* fs_par2block_get(struct snapraid_disk* disk, block_off_t parity_pos)
{
	struct snapraid_file* file;
	block_off_t file_pos;

	file = fs_par2file_get(disk, parity_pos, &file_pos);
	if (!file)
		return BLOCK_EMPTY;

	return fs_file2block_get(file, file_pos);
}

struct snapraid_map* map_alloc(const char* name, unsigned position, block_off_t total_blocks, block_off_t free_blocks, const char* uuid)
{
	struct snapraid_map* map;

	map = malloc_nofail(sizeof(struct snapraid_map));
	pathcpy(map->name, sizeof(map->name), name);
	map->position = position;
	map->total_blocks = total_blocks;
	map->free_blocks = free_blocks;
	pathcpy(map->uuid, sizeof(map->uuid), uuid);

	return map;
}

void map_free(struct snapraid_map* map)
{
	free(map);
}

int time_compare(const void* void_a, const void* void_b)
{
	const time_t* time_a = void_a;
	const time_t* time_b = void_b;

	if (*time_a < *time_b)
		return -1;
	if (*time_a > *time_b)
		return 1;
	return 0;
}
