/* radare2 - LGPL - Copyright 2017 - condret, pancake, alvaro */

#include <r_io.h>
#include <sdb.h>
#include <string.h>

R_API bool r_io_desc_init(RIO* io) {
	if (!io || io->files) {
		return false;
	}
	//fd is signed
	io->files = r_id_storage_new (3, 0x80000000);
	if (!io->files) {
		return false;
	}
	return true;
}

//shall be used by plugins for creating descs
//XXX kill mode
R_API RIODesc* r_io_desc_new(RIO* io, RIOPlugin* plugin, const char* uri, int flags, int mode, void* data) {
	ut32 fd32 = 0;
	// this is because emscript is a bitch
	if (!io || !plugin || !uri) {
		return NULL;
	}
	if (io->files) {
		if (!r_id_pool_grab_id (io->files->pool, &fd32)) {
			return NULL;
		}
	}
	RIODesc* desc = R_NEW0 (RIODesc);
	if (desc) {
		desc->fd = fd32;
		desc->io = io;
		desc->plugin = plugin;
		desc->data = data;
		desc->flags = flags;
		//because the uri-arg may live on the stack
		desc->uri = strdup (uri);
	}
	return desc;
}

R_API void r_io_desc_free(RIODesc* desc) {
	if (desc) {
		free (desc->uri);
		free (desc->referer);
		free (desc->name);
		if (desc->io && desc->io->files) {
			r_id_storage_delete (desc->io->files, desc->fd);
		}
//		free (desc->plugin);
	}
	free (desc);
}

R_API bool r_io_desc_add(RIO* io, RIODesc* desc) {
	if (!desc || !io) {
		return false;
	}
	//just for the case when plugins cannot use r_io_desc_new
	if (!desc->io) {
		desc->io = io;
	}
	if (!r_id_storage_set (io->files, desc, desc->fd)) {
		eprintf ("You are using this API incorrectly\n");
		eprintf ("fd %d was probably not generated by this RIO-instance\n", desc->fd);
		r_sys_backtrace ();
		return false;
	}
	return true;
}

R_API bool r_io_desc_del(RIO* io, int fd) {		//can we pass this a riodesc and check if it belongs to the desc->io ?
	RIODesc* desc;
	if (!io || !io->files || !(desc = r_id_storage_get (io->files, fd))) {
		return false;
	}
	r_io_desc_free (desc);
	if (desc == io->desc) {
		io->desc = NULL;
	}
	return true;
}

R_API RIODesc* r_io_desc_get(RIO* io, int fd) {
	if (!io || !io->files) {
		return NULL;
	}
	return (RIODesc*) r_id_storage_get (io->files, fd);
}

R_API RIODesc *r_io_desc_open(RIO *io, const char *uri, int flags, int mode) {
	RIOPlugin *plugin;
	RIODesc *desc;
	if (!io || !io->files || !uri) {
		return NULL;
	}
	plugin = r_io_plugin_resolve (io, uri, 0);
	if (!plugin || !plugin->open || !plugin->close) {
		return NULL;
	}
	desc = plugin->open (io, uri, flags, mode);
	if (!desc) {
		return NULL;
	}
	//for none static callbacks, those that cannot use r_io_desc_new
	if (!desc->plugin) {
		desc->plugin = plugin;
	}
	if (!desc->uri) {
		desc->uri = strdup (uri);
	}
	if (!desc->name) {
		desc->name = strdup (uri);
	}
	r_io_desc_add (io, desc);
	return desc;
}

R_API bool r_io_desc_close(RIODesc *desc) {
	RIO *io;
	if (!desc || !desc->io || !desc->plugin || !desc->plugin->close) {
		return false;
	}
	if (desc->plugin->close (desc)) {
		return false;
	}
	io = desc->io;
	// remove entry from idstorage and free the desc-struct
	r_io_desc_del (io, desc->fd);
	// remove all dead maps
	r_io_map_cleanup (io);
	r_io_section_cleanup (io);
	return true;
}

//returns length of written bytes
R_API int r_io_desc_write(RIODesc *desc, const ut8* buf, int len) {
	//check pointers
	if (!buf || !desc || !desc->plugin || !desc->plugin->write || (len < 1)) {
		return 0;
	}
	//check pointers and pcache
	if (desc->io && desc->io->p_cache) {
		return r_io_desc_cache_write (desc,
				r_io_desc_seek (desc, 0LL, R_IO_SEEK_CUR), buf, len);
	}
	//check permissions
	if (!(desc->flags & R_IO_WRITE)) {
		return 0;
	}
	return desc->plugin->write (desc->io, desc, buf, len);
}

//returns length of read bytes
R_API int r_io_desc_read(RIODesc *desc, ut8 *buf, int len) {
	ut64 seek;
	int ret;
	//check pointers and permissions
	if (!buf || !desc || !desc->plugin || !desc->plugin->read ||
	    (len < 1) || !(desc->flags & R_IO_READ)) {
		return 0;
	}
	seek = r_io_desc_seek (desc, 0LL, R_IO_SEEK_CUR);
	ret = desc->plugin->read (desc->io, desc, buf, len);
	if ((ret == len) && desc->io && desc->io->p_cache) {
		ret = r_io_desc_cache_read (desc, seek, buf, len);
	}
	return ret;
}

R_API ut64 r_io_desc_seek(RIODesc* desc, ut64 offset, int whence) {
	if (!desc || !desc->plugin || !desc->plugin->lseek) {
		return (ut64) -1;
	}
	return desc->plugin->lseek (desc->io, desc, offset, whence);
}

R_API ut64 r_io_desc_size(RIODesc* desc) {
	ut64 off, ret;
	if (!desc || !desc->plugin || !desc->plugin->lseek) {
		return 0LL;
	}
	if (r_io_desc_is_blockdevice (desc)) {
		return UT64_MAX;
	}
	off = r_io_desc_seek (desc, 0LL, R_IO_SEEK_CUR);
	ret = r_io_desc_seek (desc, 0LL, R_IO_SEEK_END);
	//what to do if that seek fails?
	r_io_desc_seek (desc, off, R_IO_SEEK_SET);
	return ret;
}

R_API bool r_io_desc_is_blockdevice (RIODesc *desc) {
	if (!desc || !desc->plugin || !desc->plugin->is_blockdevice) {
		return false;
	}
	return desc->plugin->is_blockdevice (desc);
}

R_API bool r_io_desc_exchange(RIO* io, int fd, int fdx) {
	RIODesc* desc, * descx;
	SdbListIter* iter;
	RIOMap* map;
	if (!(desc = r_io_desc_get (io, fd)) || !(descx = r_io_desc_get (io, fdx))) {
		return false;
	}
	desc->fd = fdx;
	descx->fd = fd;
	r_id_storage_set (io->files, desc,  fdx);
	r_id_storage_set (io->files, descx, fd);
	if (io->p_cache) {
		Sdb* cache = desc->cache;
		desc->cache = descx->cache;
		descx->cache = cache;
		r_io_desc_cache_cleanup (desc);
		r_io_desc_cache_cleanup (descx);
	}
	if (io->maps) {
		ls_foreach (io->maps, iter, map) {
			if (map->fd == fdx) {
				map->flags &= (desc->flags | R_IO_EXEC);
			} else if (map->fd == fd) {
				map->flags &= (descx->flags | R_IO_EXEC);
			}
		}
	}
	return true;
}

R_API bool r_io_desc_is_dbg(RIODesc *desc) {
	if (desc && desc->plugin) {
		return desc->plugin->isdbg;
	}
	return false;
}

R_API int r_io_desc_get_pid(RIODesc *desc) {
	//-1 and -2 are reserved
	if (!desc) {
		return -3;
	}
	if (!desc->plugin) {
		return -4;
	}
	if (!desc->plugin->isdbg) {
		return -5;
	}
	if (!desc->plugin->getpid) {
		return -6;
	}
	return desc->plugin->getpid (desc);
}

R_API int r_io_desc_get_tid(RIODesc *desc) {
	//-1 and -2 are reserved
	if (!desc) {
		return -3;
	}
	if (!desc->plugin) {
		return -4;
	}
	if (!desc->plugin->isdbg) {
		return -5;
	}
	if (!desc->plugin->gettid) {
		return -6;
	}
	return desc->plugin->gettid (desc);
}

R_API int r_io_desc_read_at(RIODesc *desc, ut64 addr, ut8 *buf, int len) {
	if (desc && buf && (r_io_desc_seek (desc, addr, R_IO_SEEK_SET) == addr)) {
		return r_io_desc_read (desc, buf, len);
	}
	return 0;
}

R_API int r_io_desc_write_at(RIODesc *desc, ut64 addr, const ut8 *buf, int len) {
	if (desc && buf && (r_io_desc_seek (desc, addr, R_IO_SEEK_SET) == addr)) {
		return r_io_desc_write (desc, buf, len);
	}
	return 0;
}

static bool desc_fini_cb(void* user, void* data, ut32 id) {
	RIODesc* desc = (RIODesc*) data;
	if (desc->plugin && desc->plugin->close) {
		desc->plugin->close (desc);
	}
	return true;
}

//closes all descs and frees all descs and io->files
R_API bool r_io_desc_fini(RIO* io) {
	if (!io || !io->files) {
		return false;
	}
	r_id_storage_foreach (io->files, desc_fini_cb, io);
	r_id_storage_free (io->files);
	io->files = NULL;
	//no map-cleanup here, to keep it modular useable
	io->desc = NULL;
	return true;
}
