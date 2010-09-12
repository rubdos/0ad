/* Copyright (C) 2010 Wildfire Games.
 * This file is part of 0 A.D.
 *
 * 0 A.D. is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * 0 A.D. is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with 0 A.D.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "precompiled.h"

#include "TextureManager.h"

#include "graphics/TextureConverter.h"
#include "lib/allocators/shared_ptr.h"
#include "lib/res/h_mgr.h"
#include "lib/file/vfs/vfs_tree.h"
#include "lib/res/graphics/ogl_tex.h"
#include "lib/timer.h"
#include "maths/MD5.h"
#include "ps/CLogger.h"
#include "ps/Filesystem.h"

// Comparison functor that operates over texture properties, or
// over the properties of a CTexturePtr (ignoring the mutable state like Handle).
struct TextureCacheCmp
{
	bool operator()(const CTexturePtr& a, const CTexturePtr& b) const
	{
		return (*this)(a->m_Properties, b->m_Properties);
	}

	bool operator()(const CTextureProperties& a, const CTextureProperties& b) const
	{
		if (a.m_Path < b.m_Path)
			return true;
		if (b.m_Path < a.m_Path)
			return false;

		if (a.m_Filter < b.m_Filter)
			return true;
		if (b.m_Filter < a.m_Filter)
			return false;

		if (a.m_Wrap < b.m_Wrap)
			return true;
		if (b.m_Wrap < a.m_Wrap)
			return false;

		if (a.m_Aniso < b.m_Aniso)
			return true;
		if (b.m_Aniso < a.m_Aniso)
			return false;

		return false;
	}
};

class CTextureManagerImpl
{
	friend class CTexture;
public:
	CTextureManagerImpl(PIVFS vfs, bool disableGL)
		: m_VFS(vfs), m_DisableGL(disableGL), m_TextureConverter(vfs), m_DefaultHandle(0), m_ErrorHandle(0)
	{
		// Initialise some textures that will always be available,
		// without needing to load any files

		// Default placeholder texture (grey)
		if (!m_DisableGL)
		{
			// Construct 1x1 24-bit texture
			shared_ptr<u8> data(new u8[3], ArrayDeleter());
			data.get()[0] = 64;
			data.get()[1] = 64;
			data.get()[2] = 64;
			Tex t;
			(void)tex_wrap(1, 1, 24, 0, data, 0, &t);

			m_DefaultHandle = ogl_tex_wrap(&t, m_VFS, L"(default texture)");
			(void)ogl_tex_set_filter(m_DefaultHandle, GL_LINEAR);
			if (!m_DisableGL)
				(void)ogl_tex_upload(m_DefaultHandle);
		}

		// Error texture (magenta)
		if (!m_DisableGL)
		{
			// Construct 1x1 24-bit texture
			shared_ptr<u8> data(new u8[3], ArrayDeleter());
			data.get()[0] = 255;
			data.get()[1] = 0;
			data.get()[2] = 255;
			Tex t;
			(void)tex_wrap(1, 1, 24, 0, data, 0, &t);

			m_ErrorHandle = ogl_tex_wrap(&t, m_VFS, L"(error texture)");
			(void)ogl_tex_set_filter(m_ErrorHandle, GL_LINEAR);
			if (!m_DisableGL)
				(void)ogl_tex_upload(m_ErrorHandle);

			// Construct a CTexture to return to callers who want an error texture
			CTextureProperties props(L"(error texture)");
			m_ErrorTexture = CTexturePtr(new CTexture(m_ErrorHandle, props, this));
			m_ErrorTexture->m_State = CTexture::LOADED;
			m_ErrorTexture->m_Self = m_ErrorTexture;
		}

		// Allow hotloading of textures
		RegisterFileReloadFunc(ReloadChangedFileCB, this);
	}

	~CTextureManagerImpl()
	{
		UnregisterFileReloadFunc(ReloadChangedFileCB, this);

		(void)ogl_tex_free(m_DefaultHandle);
		(void)ogl_tex_free(m_ErrorHandle);
	}

	CTexturePtr GetErrorTexture()
	{
		return m_ErrorTexture;
	}

	/**
	 * See CTextureManager::CreateTexture
	 */
	CTexturePtr CreateTexture(const CTextureProperties& props)
	{
		// Construct a new default texture with the given properties to use as the search key
		CTexturePtr texture(new CTexture(m_DefaultHandle, props, this));

		// Try to find an existing texture with the given properties
		TextureCache::iterator it = m_TextureCache.find(texture);
		if (it != m_TextureCache.end())
			return *it;

		// Can't find an existing texture - finish setting up this new texture
		texture->m_Self = texture;
		m_TextureCache.insert(texture);
		m_HotloadFiles[props.m_Path].insert(texture);

		return texture;
	}

	/**
	 * Load the given file into the texture object and upload it to OpenGL.
	 * Assumes the file already exists.
	 */
	void LoadTexture(const CTexturePtr& texture, const VfsPath& path)
	{
		if (m_DisableGL)
			return;

		Handle h = ogl_tex_load(m_VFS, path, RES_UNIQUE);
		if (h <= 0)
		{
			LOGERROR(L"Texture failed to load; \"%ls\"", texture->m_Properties.m_Path.string().c_str());

			// Replace with error texture to make it obvious
			texture->SetHandle(m_ErrorHandle);
			return;
		}

		// Get some flags for later use
		size_t flags = 0;
		(void)ogl_tex_get_format(h, &flags, NULL);

		// Initialise base colour from the texture
		(void)ogl_tex_get_average_colour(h, &texture->m_BaseColour);

		// Set GL upload properties
		(void)ogl_tex_set_wrap(h, texture->m_Properties.m_Wrap);
		(void)ogl_tex_set_anisotropy(h, texture->m_Properties.m_Aniso);

		// Prevent ogl_tex automatically generating mipmaps (which is slow and unwanted),
		// by avoiding mipmapped filters unless the source texture already has mipmaps
		GLint filter = texture->m_Properties.m_Filter;
		if (!(flags & TEX_MIPMAPS))
		{
			switch (filter)
			{
			case GL_NEAREST_MIPMAP_NEAREST:
			case GL_NEAREST_MIPMAP_LINEAR:
				filter = GL_NEAREST;
				break;
			case GL_LINEAR_MIPMAP_NEAREST:
			case GL_LINEAR_MIPMAP_LINEAR:
				filter = GL_LINEAR;
				break;
			}
		}
		(void)ogl_tex_set_filter(h, filter);

		// Upload to GL
		if (!m_DisableGL && ogl_tex_upload(h) < 0)
		{
			LOGERROR(L"Texture failed to upload: \"%ls\"", texture->m_Properties.m_Path.string().c_str());

			ogl_tex_free(h);

			// Replace with error texture to make it obvious
			texture->SetHandle(m_ErrorHandle);
			return;
		}

		// Let the texture object take ownership of this handle
		texture->SetHandle(h, true);
	}

	/**
	 * Determines whether we can safely use the archived cache file, or need to
	 * re-convert the source file.
	 */
	bool CanUseArchiveCache(const VfsPath& sourcePath, const VfsPath& archiveCachePath)
	{
		// We want to use the archive cache whenever possible,
		// unless it's superseded by a source file that the user has edited

		size_t sourcePriority = 0;
		size_t archiveCachePriority = 0;

		bool sourceExists = (m_VFS->GetFilePriority(sourcePath, &sourcePriority) >= 0);
		bool archiveCacheExists = (m_VFS->GetFilePriority(archiveCachePath, &archiveCachePriority) >= 0);

		// Can't use it if there's no cache
		if (!archiveCacheExists)
			return false;

		// Must use the cache if there's no source
		if (!sourceExists)
			return true;

		// If source file is from a higher-priority mod than archive cache,
		// don't use the old cache
		if (archiveCachePriority < sourcePriority)
			return false;

		// If source file is more recent than the archive cache (i.e. the user has edited it),
		// don't use the old cache
		FileInfo sourceInfo, archiveCacheInfo;
		if (m_VFS->GetFileInfo(sourcePath, &sourceInfo) >= 0 &&
		    m_VFS->GetFileInfo(archiveCachePath, &archiveCacheInfo) >= 0)
		{
			const double howMuchNewer = difftime(sourceInfo.MTime(), archiveCacheInfo.MTime());
			const double threshold = 2.0;	// FAT timestamp resolution [seconds]
			if (howMuchNewer > threshold)
				return false;
		}

		// Otherwise we can use the cache
		return true;
	}

	/**
	 * Attempts to load a cached version of a texture.
	 * If the texture is loaded (or there was an error), returns true.
	 * Otherwise, returns false to indicate the caller should generate the cached version.
	 */
	bool TryLoadingCached(const CTexturePtr& texture)
	{
		VfsPath sourcePath = texture->m_Properties.m_Path;
		VfsPath sourceDir = sourcePath.branch_path();
		std::wstring sourceName = sourcePath.leaf();
		VfsPath archiveCachePath = sourceDir / (sourceName + L".dds");

		// Try the archive cache file first
		if (CanUseArchiveCache(sourcePath, archiveCachePath))
		{
			LoadTexture(texture, archiveCachePath);
			return true;
		}

		// Fail if no source or archive cache
		if (m_VFS->GetFileInfo(sourcePath, NULL) < 0)
		{
			LOGERROR(L"Texture failed to find source file: \"%ls\"", texture->m_Properties.m_Path.string().c_str());

			texture->SetHandle(m_ErrorHandle);
			return true;
		}

		// Look for loose cache of source file

		VfsPath looseCachePath = LooseCachePath(texture);

		// If the loose cache file exists, use it
		if (m_VFS->GetFileInfo(looseCachePath, NULL) >= 0)
		{
			LoadTexture(texture, looseCachePath);
			return true;
		}

		// No cache - we'll need to regenerate it

		return false;
	}

	/**
	 * Returns the pathname for storing a loose cache file, based on the size/mtime of
	 * the source file and the conversion settings. The source file must already exist.
	 *
	 * TODO: this code should probably be shared with other cached data (XMB files etc).
	 */
	VfsPath LooseCachePath(const CTexturePtr& texture)
	{
		VfsPath sourcePath = texture->m_Properties.m_Path;

		FileInfo fileInfo;
		if (m_VFS->GetFileInfo(sourcePath, &fileInfo) < 0)
		{
			debug_warn(L"source file disappeared"); // this should never happen
			return VfsPath();
		}

		u64 mtime = (u64)fileInfo.MTime() & ~1; // skip lowest bit, since zip and FAT don't preserve it
		u64 size = (u64)fileInfo.Size();

		u32 version = 0; // change this if we update the code and need to invalidate old users' caches

		// Construct a hash of the file data and settings.

		CTextureConverter::Settings settings = GetConverterSettings(texture);

		MD5 hash;
		hash.Update((const u8*)&mtime, sizeof(mtime));
		hash.Update((const u8*)&size, sizeof(size));
		hash.Update((const u8*)&version, sizeof(version));
		settings.Hash(hash);
		// these are local cached files, so we don't care about endianness etc

		// Use a short prefix of the full hash (we don't need high collision-resistance),
		// converted to hex
		u8 digest[MD5::DIGESTSIZE];
		hash.Final(digest);
		std::wstringstream digestPrefix;
		digestPrefix << std::hex;
		for (size_t i = 0; i < 8; ++i)
			digestPrefix << std::setfill(L'0') << std::setw(2) << (int)digest[i];

		// Construct the final path
		VfsPath sourceDir = sourcePath.branch_path();
		std::wstring sourceName = sourcePath.leaf();
		return L"cache" / sourceDir / (sourceName + L"." + digestPrefix.str() + L".dds");

		// TODO: we should probably include the mod name, once that's possible (http://trac.wildfiregames.com/ticket/564)
	}

	/**
	 * Initiates an asynchronous conversion process, from the texture's
	 * source file to the corresponding loose cache file.
	 */
	void ConvertTexture(const CTexturePtr& texture)
	{
		VfsPath sourcePath = texture->m_Properties.m_Path;
		VfsPath looseCachePath = LooseCachePath(texture);

//		LOGWARNING(L"Converting texture \"%ls\"", srcPath.string().c_str());

		CTextureConverter::Settings settings = GetConverterSettings(texture);

		m_TextureConverter.ConvertTexture(texture, sourcePath, looseCachePath, settings);
	}

	bool MakeProgress()
	{
		// Process any completed conversion tasks
		{
			CTexturePtr texture;
			VfsPath dest;
			bool ok;
			if (m_TextureConverter.Poll(texture, dest, ok))
			{
				if (ok)
				{
					LoadTexture(texture, dest);
				}
				else
				{
					LOGERROR(L"Texture failed to convert: \"%ls\"", texture->m_Properties.m_Path.string().c_str());
					texture->SetHandle(m_ErrorHandle);
				}
				texture->m_State = CTexture::LOADED;
				return true;
			}
		}

		// We'll only push new conversion requests if it's not already busy
		bool converterBusy = m_TextureConverter.IsBusy();

		if (!converterBusy)
		{
			// Look for all high-priority textures needing conversion.
			// (Iterating over all textures isn't optimally efficient, but it
			// doesn't seem to be a problem yet and it's simpler than maintaining
			// multiple queues.)
			for (TextureCache::iterator it = m_TextureCache.begin(); it != m_TextureCache.end(); ++it)
			{
				if ((*it)->m_State == CTexture::HIGH_NEEDS_CONVERTING)
				{
					// Start converting this texture
					(*it)->m_State = CTexture::HIGH_IS_CONVERTING;
					ConvertTexture(*it);
					return true;
				}
			}
		}

		// Try loading prefetched textures from their cache
		for (TextureCache::iterator it = m_TextureCache.begin(); it != m_TextureCache.end(); ++it)
		{
			if ((*it)->m_State == CTexture::PREFETCH_NEEDS_LOADING)
			{
				if (TryLoadingCached(*it))
				{
					(*it)->m_State = CTexture::LOADED;
				}
				else
				{
					(*it)->m_State = CTexture::PREFETCH_NEEDS_CONVERTING;
				}
				return true;
			}
		}

		// If we've got nothing better to do, then start converting prefetched textures.
		if (!converterBusy)
		{
			for (TextureCache::iterator it = m_TextureCache.begin(); it != m_TextureCache.end(); ++it)
			{
				if ((*it)->m_State == CTexture::PREFETCH_NEEDS_CONVERTING)
				{
					(*it)->m_State = CTexture::PREFETCH_IS_CONVERTING;
					ConvertTexture(*it);
					return true;
				}
			}
		}

		return false;
	}

	/**
	 * Compute the conversion settings that apply to a given texture, by combining
	 * the textures.xml files from its directory and all parent directories
	 * (up to the VFS root).
	 */
	CTextureConverter::Settings GetConverterSettings(const CTexturePtr& texture)
	{
		VfsPath srcPath = texture->m_Properties.m_Path;

		std::vector<CTextureConverter::SettingsFile*> files;
		VfsPath p;
		for (VfsPath::iterator it = srcPath.begin(); it != srcPath.end(); ++it)
		{
			VfsPath settingsPath = p/L"textures.xml";
			m_HotloadFiles[settingsPath].insert(texture);
			CTextureConverter::SettingsFile* f = GetSettingsFile(settingsPath);
			if (f)
				files.push_back(f);
			p /= *it;
		}
		return m_TextureConverter.ComputeSettings(srcPath.leaf(), files);
	}

	/**
	 * Return the (cached) settings file with the given filename,
	 * or NULL if it doesn't exist.
	 */
	CTextureConverter::SettingsFile* GetSettingsFile(const VfsPath& path)
	{
		SettingsFilesMap::iterator it = m_SettingsFiles.find(path);
		if (it != m_SettingsFiles.end())
			return it->second.get();

		if (m_VFS->GetFileInfo(path, NULL) >= 0)
		{
			shared_ptr<CTextureConverter::SettingsFile> settings(m_TextureConverter.LoadSettings(path));
			m_SettingsFiles.insert(std::make_pair(path, settings));
			return settings.get();
		}
		else
		{
			m_SettingsFiles.insert(std::make_pair(path, shared_ptr<CTextureConverter::SettingsFile>()));
			return NULL;
		}
	}

	static LibError ReloadChangedFileCB(void* param, const VfsPath& path)
	{
		return static_cast<CTextureManagerImpl*>(param)->ReloadChangedFile(path);
	}

	LibError ReloadChangedFile(const VfsPath& path)
	{
		// Uncache settings file, if this is one
		m_SettingsFiles.erase(path);

		// Find all textures using this file
		std::map<VfsPath, std::set<boost::weak_ptr<CTexture> > >::iterator files = m_HotloadFiles.find(path);
		if (files != m_HotloadFiles.end())
		{
			// Flag all textures using this file as needing reloading
			for (std::set<boost::weak_ptr<CTexture> >::iterator it = files->second.begin(); it != files->second.end(); ++it)
			{
				if (shared_ptr<CTexture> texture = it->lock())
				{
					texture->m_State = CTexture::UNLOADED;
					texture->SetHandle(m_DefaultHandle);
				}
			}
		}

		return INFO::OK;
	}

private:
	PIVFS m_VFS;
	bool m_DisableGL;
	CTextureConverter m_TextureConverter;

	Handle m_DefaultHandle;
	Handle m_ErrorHandle;
	CTexturePtr m_ErrorTexture;

	// Cache of all loaded textures
	typedef std::set<CTexturePtr, TextureCacheCmp> TextureCache;
	TextureCache m_TextureCache;
	// TODO: we ought to expire unused textures from the cache eventually

	// Store the set of textures that need to be reloaded when the given file
	// (a source file or settings.xml) is modified
	std::map<VfsPath, std::set<boost::weak_ptr<CTexture> > > m_HotloadFiles;

	// Cache for the conversion settings files
	typedef std::map<VfsPath, shared_ptr<CTextureConverter::SettingsFile> > SettingsFilesMap;
	SettingsFilesMap m_SettingsFiles;
};


CTexture::CTexture(Handle handle, const CTextureProperties& props, CTextureManagerImpl* textureManager) :
	m_Handle(handle), m_BaseColour(0), m_State(UNLOADED), m_Properties(props), m_TextureManager(textureManager)
{
	// Add a reference to the handle (it might be shared by multiple CTextures
	// so we can't take ownership of it)
	if (m_Handle)
		h_add_ref(m_Handle);
}

CTexture::~CTexture()
{
	if (m_Handle)
		ogl_tex_free(m_Handle);
}

void CTexture::Bind(size_t unit)
{
	// TODO: TryLoad might call ogl_tex_upload which enables GL_TEXTURE_2D
	// on texture unit 0, regardless of 'unit', which callers might
	// not be expecting. Ideally that wouldn't happen.

	TryLoad();

	ogl_tex_bind(m_Handle, unit);
}

bool CTexture::TryLoad()
{
	// If we haven't started loading, then try loading, and if that fails then request conversion.
	// If we have already tried prefetch loading, and it failed, bump the conversion request to HIGH priority.
	if (m_State == UNLOADED || m_State == PREFETCH_NEEDS_LOADING || m_State == PREFETCH_NEEDS_CONVERTING)
	{
		if (shared_ptr<CTexture> self = m_Self.lock())
		{
			if (m_State != PREFETCH_NEEDS_CONVERTING && m_TextureManager->TryLoadingCached(self))
				m_State = LOADED;
			else
				m_State = HIGH_NEEDS_CONVERTING;
		}
	}

	return (m_State == LOADED);
}

void CTexture::Prefetch()
{
	if (m_State == UNLOADED)
	{
		if (shared_ptr<CTexture> self = m_Self.lock())
		{
			m_State = PREFETCH_NEEDS_LOADING;
		}
	}
}

bool CTexture::IsLoaded()
{
	return (m_State == LOADED);
}

void CTexture::SetHandle(Handle handle, bool takeOwnership)
{
	if (handle == m_Handle)
		return;

	if (!takeOwnership)
		h_add_ref(handle);

	ogl_tex_free(m_Handle);
	m_Handle = handle;
}

size_t CTexture::GetWidth() const
{
	size_t w = 0;
	(void)ogl_tex_get_size(m_Handle, &w, 0, 0);
	return w;
}

size_t CTexture::GetHeight() const
{
	size_t h = 0;
	(void)ogl_tex_get_size(m_Handle, 0, &h, 0);
	return h;
}

bool CTexture::HasAlpha() const
{
	size_t flags = 0;
	(void)ogl_tex_get_format(m_Handle, &flags, 0);
	return (flags & TEX_ALPHA) != 0;
}

u32 CTexture::GetBaseColour() const
{
	return m_BaseColour;
}


// CTextureManager: forward all calls to impl:

CTextureManager::CTextureManager(PIVFS vfs, bool disableGL) :
	m(new CTextureManagerImpl(vfs, disableGL))
{
}

CTextureManager::~CTextureManager()
{
	delete m;
}

CTexturePtr CTextureManager::CreateTexture(const CTextureProperties& props)
{
	return m->CreateTexture(props);
}

CTexturePtr CTextureManager::GetErrorTexture()
{
	return m->GetErrorTexture();
}

bool CTextureManager::MakeProgress()
{
	return m->MakeProgress();
}
