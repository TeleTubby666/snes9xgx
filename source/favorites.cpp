/****************************************************************************
 * Snes9x GX - Favorites
 *
 * favorites.cpp
 *
 * Favorites are stored as one full ROM path per line in
 * <save device>:/snes9xgx/favorites.txt, e.g.
 *     sd:/snes9xgx/roms/Contra III.smc
 *
 * Note: plain fopen() is used on purpose instead of LoadFile()/SaveFile() -
 * those call HaltParseThread(), which would cut short the background scan of
 * the folder the user is currently browsing.
 ***************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <string>
#include <vector>
#include <algorithm>

#include "snes9xgx.h"
#include "filebrowser.h"
#include "favorites.h"
#include "fileop.h"
#include "menu.h"
#include "drivers/Platform.h"
#include "drivers/FileSystemDriver.h"

#define MAX_FAVORITES   400
#define FAV_FILE_MAX    (64 * 1024)

static std::vector<std::string> favorites;
static bool favoritesLoaded = false;
static char favoritesPathLoaded[MAXPATHLEN] = { 0 };

// snapshot of the list that SaveFavoritesTask() writes
static std::string pendingSave;
static bool favoritesDirty = false;

/****************************************************************************
 * Small helpers
 ***************************************************************************/
static bool GetFavoritesPath(char * path)
{
	if(EmuSettings.saveDevice == DEVICE_AUTO)
		return false;
	platform->getFileSystem()->getPath(path, MAXPATHLEN, EmuSettings.saveDevice, APPFOLDER, FAVORITES_FILE);
	return true;
}

static int FindFavorite(const char * fullpath)
{
	for(size_t i = 0; i < favorites.size(); i++)
	{
		if(strcasecmp(favorites[i].c_str(), fullpath) == 0)
			return (int)i;
	}
	return -1;
}

// "sd:/snes9xgx/roms/Game.smc" -> dir "sd:/snes9xgx/roms/", name "Game.smc"
static bool SplitPath(const char * full, char * dir, size_t dirSize, char * name, size_t nameSize)
{
	const char * slash = strrchr(full, '/');
	if(slash == NULL || slash[1] == 0)
		return false;

	size_t dirLen = (size_t)(slash - full) + 1;
	if(dirLen >= dirSize || strlen(slash + 1) >= nameSize)
		return false;

	memcpy(dir, full, dirLen);
	dir[dirLen] = 0;
	snprintf(name, nameSize, "%s", slash + 1);
	return true;
}

static bool HasRomExtension(const char * filename)
{
	const char * p = strrchr(filename, '.');
	if(p == NULL)
		return false;

	return strcasecmp(p, ".smc") == 0 || strcasecmp(p, ".sfc") == 0 ||
	       strcasecmp(p, ".fig") == 0 || strcasecmp(p, ".swc") == 0 ||
	       strcasecmp(p, ".bs")  == 0 || strcasecmp(p, ".zip") == 0;
}

static bool IsSelectionValid()
{
	return browser.numEntries > 0 && browser.selIndex >= 0 && browser.selIndex < browser.numEntries;
}

static BROWSERENTRY * FindBrowserEntry(const char * filename)
{
	for(int i = 0; i < browser.numEntries; i++)
	{
		if(strcmp(browserList[i].filename, filename) == 0)
			return &browserList[i];
	}
	return NULL;
}

/****************************************************************************
 * Load / save the list (no HaltParseThread - see header comment)
 ***************************************************************************/
static void EnsureLoaded()
{
	char path[MAXPATHLEN];
	if(!GetFavoritesPath(path))
		return; // save device not known yet - try again later

	// already loaded from this very location?
	if(favoritesLoaded && strcmp(path, favoritesPathLoaded) == 0)
		return;

	snprintf(favoritesPathLoaded, sizeof(favoritesPathLoaded), "%s", path);
	favoritesLoaded = true;
	favorites.clear();

	if(!ChangeInterface(path, SILENT))
		return;

	FILE * fp = fopen(path, "rb");
	if(fp == NULL)
		return; // no favorites yet

	char * buf = (char *)malloc(FAV_FILE_MAX + 1);
	if(buf == NULL)
	{
		fclose(fp);
		return;
	}

	size_t size = fread(buf, 1, FAV_FILE_MAX, fp);
	fclose(fp);
	buf[size] = 0;

	char * line = buf;
	while(line != NULL && *line != 0 && favorites.size() < MAX_FAVORITES)
	{
		char * next = strchr(line, '\n');
		if(next != NULL)
			*next++ = 0;

		size_t len = strlen(line);
		while(len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' '))
			line[--len] = 0;

		if(len > 0 && len < MAXJOLIET && FindFavorite(line) < 0)
			favorites.push_back(line);

		line = next;
	}
	free(buf);
}

static void BuildPendingSave()
{
	pendingSave.clear();
	for(size_t i = 0; i < favorites.size(); i++)
	{
		pendingSave += favorites[i];
		pendingSave += '\n';
	}
	if(pendingSave.empty())
		pendingSave = "\n"; // never write a 0-byte file
	favoritesDirty = true;
}

int FavoritesLoadTask(void *)
{
	EnsureLoaded();
	return 1;
}

int SaveFavoritesTask(void *)
{
	if(!favoritesDirty)
		return 1;

	char path[MAXPATHLEN];
	if(!GetFavoritesPath(path))
		return 0;

	if(!ChangeInterface(path, SILENT))
		return 0;

	FILE * fp = fopen(path, "wb");
	if(fp == NULL)
		return 0;

	size_t len = pendingSave.size();
	bool ok = fwrite(pendingSave.data(), 1, len, fp) == len;
	if(fclose(fp) != 0)
		ok = false;

	if(ok)
		favoritesDirty = false;
	return ok ? 1 : 0;
}

/****************************************************************************
 * Favorites view
 ***************************************************************************/
bool InFavoritesView()
{
	return strcmp(browser.dir, FAVORITES_DIR) == 0;
}

struct FavSortItem
{
	std::string display;
	size_t index;
};

static bool FavSortLess(const FavSortItem & a, const FavSortItem & b)
{
	return strcasecmp(a.display.c_str(), b.display.c_str()) < 0;
}

static int ShowFavorites()
{
	EnsureLoaded();
	HaltParseThread();
	ResetBrowser();
	snprintf(browser.dir, sizeof(browser.dir), "%s", FAVORITES_DIR);

	// ".." entry so the user can go back to the device listing
	if(!AddBrowserEntry())
		return 0;
	sprintf(browserList[0].filename, "..");
	sprintf(browserList[0].displayname, "Up One Level");
	browserList[0].isdir = 1;
	browserList[0].icon = ICON_FOLDER;

	std::vector<FavSortItem> items;
	for(size_t i = 0; i < favorites.size(); i++)
	{
		char dir[MAXPATHLEN + 1];
		char name[MAXJOLIET + 1];
		char utf8Name[MAXJOLIET + 1];
		char display[MAXJOLIET + 1];

		if(!SplitPath(favorites[i].c_str(), dir, sizeof(dir), name, sizeof(name)))
			continue;
		if(!HasRomExtension(name)) // eg. a hand-edited .7z line - can't be loaded from here
			continue;

		platform->getFileSystem()->nameToDisplay(name, utf8Name, sizeof(utf8Name));
		StripExt(display, utf8Name);

		FavSortItem item;
		item.display = display;
		item.index = i;
		items.push_back(item);
	}
	std::sort(items.begin(), items.end(), FavSortLess);

	for(size_t i = 0; i < items.size(); i++)
	{
		if(!AddBrowserEntry())
			break;
		BROWSERENTRY & e = browserList[browser.size - 1];
		snprintf(e.filename, sizeof(e.filename), "%s", favorites[items[i].index].c_str());
		snprintf(e.displayname, sizeof(e.displayname), "%s", items[i].display.c_str());
		e.isdir = 0;
		e.icon = ICON_NONE;
	}

	browser.numEntries = browser.size;
	browser.selIndex = 0;
	browser.pageIndex = 0;
	return browser.numEntries;
}

static int LeaveFavorites()
{
	HaltParseThread();
	browser.dir[0] = 0;
	ResetBrowser();
	browser.numEntries = AddDeviceListing();

	// same as BrowserChangeFolder() does for the device listing
	EmuSettings.loadFolder[0] = 0;
	EmuSettings.loadDevice = DEVICE_AUTO;
	return browser.numEntries;
}

bool FavoritesChangeFolder(int * result)
{
	if(!IsSelectionValid())
		return false;

	const char * selected = browserList[browser.selIndex].filename;

	// click on "Favorites" in the device listing
	if(browser.dir[0] == 0 && strcmp(selected, FAVORITES_DIR) == 0)
	{
		*result = ShowFavorites();
		return true;
	}

	// click on ".." inside the favorites view
	if(InFavoritesView() && strcmp(selected, "..") == 0)
	{
		*result = LeaveFavorites();
		return true;
	}

	return false;
}

int AddFavoritesListingEntry(int index)
{
	if(!AddBrowserEntry())
		return 0;

	sprintf(browserList[index].filename, "%s", FAVORITES_DIR);
	sprintf(browserList[index].displayname, "Favorites");
	browserList[index].length = 0;
	browserList[index].isdir = 1;
	browserList[index].icon = ICON_FOLDER;
	return 1;
}

/****************************************************************************
 * Loading a favorite
 *
 * BrowserLoadFile() and ROMLoader() work on browser.dir + selected filename,
 * so both are pointed at the real location of the ROM first. On failure the
 * list is NOT rebuilt (the GUI may be reading it) - the one entry we touched
 * and browser.dir are simply put back.
 ***************************************************************************/
int BrowserLoadFavorite()
{
	if(!IsSelectionValid() || browserList[browser.selIndex].isdir)
		return 0;

	int sel = browser.selIndex;
	char full[MAXJOLIET + 1];
	char dir[MAXPATHLEN + 1];
	char name[MAXJOLIET + 1];

	snprintf(full, sizeof(full), "%s", browserList[sel].filename);

	if(!SplitPath(full, dir, sizeof(dir), name, sizeof(name)))
	{
		ErrorPrompt("Invalid favorite entry!");
		return 0;
	}

	snprintf(browser.dir, sizeof(browser.dir), "%s", dir);
	int loaded = 0;

	if(ChangeInterface(browser.dir, NOTSILENT))
	{
		snprintf(browserList[sel].filename, sizeof(browserList[sel].filename), "%s", name);
		loaded = BrowserLoadFile(); // browser.dir is no longer the favorites dir
	}

	if(loaded)
	{
		// remember the ROM's folder so the normal browser opens there next time
		const char * path = StripDevice(browser.dir);
		if(path != NULL)
		{
			strncpy(EmuSettings.loadFolder, path, sizeof(EmuSettings.loadFolder) - 1);
			EmuSettings.loadFolder[sizeof(EmuSettings.loadFolder) - 1] = 0;
		}
		FindDevice(browser.dir, &EmuSettings.loadDevice);
	}
	else
	{
		snprintf(browserList[sel].filename, sizeof(browserList[sel].filename), "%s", full);
		snprintf(browser.dir, sizeof(browser.dir), "%s", FAVORITES_DIR);
	}
	return loaded;
}

/****************************************************************************
 * Toggle favorite state of the selected entry (memory only)
 ***************************************************************************/
int ToggleFavoriteOnSelection()
{
	EnsureLoaded();

	if(!IsSelectionValid())
		return FAVORITE_NOT_USABLE;

	// snapshot - a background directory scan may re-sort browserList under us
	char filename[MAXJOLIET + 1];
	snprintf(filename, sizeof(filename), "%s", browserList[browser.selIndex].filename);
	bool isdir = browserList[browser.selIndex].isdir != 0;

	// favorites view: the button removes the entry
	if(InFavoritesView())
	{
		if(isdir)
			return FAVORITE_NOT_USABLE;

		int idx = FindFavorite(filename);
		if(idx < 0)
			return FAVORITE_NOT_USABLE;

		int oldSel = browser.selIndex;
		favorites.erase(favorites.begin() + idx);
		BuildPendingSave();
		ShowFavorites();
		browser.selIndex = (oldSel < browser.numEntries) ? oldSel : browser.numEntries - 1;
		return FAVORITE_REMOVED;
	}

	// normal browser: only real ROM files, not the contents of a 7z
	if(inSz || browser.dir[0] == 0 || isdir || !HasRomExtension(filename))
		return FAVORITE_NOT_USABLE;

	if(strlen(browser.dir) + strlen(filename) >= MAXJOLIET)
		return FAVORITE_NOT_USABLE;

	char full[MAXPATHLEN + 1];
	snprintf(full, sizeof(full), "%s%s", browser.dir, filename);

	BROWSERENTRY * e = FindBrowserEntry(filename);
	int idx = FindFavorite(full);

	if(idx >= 0)
	{
		favorites.erase(favorites.begin() + idx);
		BuildPendingSave();
		if(e != NULL && strncmp(e->displayname, "* ", 2) == 0)
			memmove(e->displayname, e->displayname + 2, strlen(e->displayname + 2) + 1);
		return FAVORITE_REMOVED;
	}

	if(favorites.size() >= MAX_FAVORITES)
		return FAVORITE_LIST_FULL;

	favorites.push_back(full);
	BuildPendingSave();
	if(e != NULL && strncmp(e->displayname, "* ", 2) != 0 && strlen(e->displayname) + 2 < sizeof(e->displayname))
	{
		memmove(e->displayname + 2, e->displayname, strlen(e->displayname) + 1);
		e->displayname[0] = '*';
		e->displayname[1] = ' ';
	}
	return FAVORITE_ADDED;
}
