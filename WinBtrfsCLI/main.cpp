/* main.cpp
 * CLI processing and Dokan stuff
 *
 * WinBtrfs
 *
 * Copyright (c) 2011 Justin Gottula
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <cassert>
#include <vector>
#include <boost/detail/endian.hpp>
#include "btrfs_system.h"
#include "chunktree_parser.h"
#include "dokan_callbacks.h"
#include "endian.h"
#include "fstree_parser.h"
#include "roottree_parser.h"

WCHAR devicePath[MAX_PATH], mountPoint[MAX_PATH];
DOKAN_OPERATIONS btrfsOperations = {
	&btrfsCreateFile,
	&btrfsOpenDirectory,
	&btrfsCreateDirectory,
	&btrfsCleanup,
	&btrfsCloseFile,
	&btrfsReadFile,
	&btrfsWriteFile,
	&btrfsFlushFileBuffers,
	&btrfsGetFileInformation,
	&btrfsFindFiles,
	NULL, // FindFilesWithPattern
	&btrfsSetFileAttributes,
	&btrfsSetFileTime,
	&btrfsDeleteFile,
	&btrfsDeleteDirectory,
	&btrfsMoveFile,
	&btrfsSetEndOfFile,
	&btrfsSetAllocationSize,
	&btrfsLockFile,
	&btrfsUnlockFile,
	&btrfsGetDiskFreeSpace,
	&btrfsGetVolumeInformation,
	&btrfsUnmount,
	&btrfsGetFileSecurity,
	&btrfsSetFileSecurity
};

extern BtrfsObjID mountedSubvol;
extern BtrfsSuperblock super;
extern std::vector<KeyedItem> rootTree;

bool useSubvolID = false, useSubvolName = false;
BtrfsObjID subvolID;
char *subvolName;
bool noDump = false;

void firstTasks()
{
	DWORD errorCode;

#ifndef BOOST_LITTLE_ENDIAN
		printf("firstTasks: warning, support for non-little-endian architectures is untested!\n");
#endif

	if ((errorCode = init()) != 0)
	{
		printf("firstTasks: failed to get a handle on the partition! (GetLastError: %d)\n", errorCode);

		exit(1);
	}

	if ((errorCode = setupBigDokanLock()) != 0)
	{
		printf("firstTasks: failed to setup the Big Dokan Lock! (GetLastError: %d)\n", errorCode);

		exit(1);
	}

	if ((errorCode = readPrimarySB()) != 0)
	{
		printf("firstTasks: failed to read the primary superblock! (GetLastError: %d)\n", errorCode);

		cleanUp();
		exit(1);
	}

	switch (validateSB(NULL))
	{
	case 0:
		/* quiet on successful SB validation */
		break;
	case 1:
		printf("firstTasks: superblock is missing or invalid!\n");
		cleanUp();
		exit(1);
	case 2:
		printf("firstTasks: superblock checksum failed!\n");
		cleanUp();
		exit(1);
	default:
		printf("firstTasks: superblock failed to validate for an unknown reason!\n");
		cleanUp();
		exit(1);
	}

	findSecondarySBs();

	if (super.numDevices > 1)
		printf("firstTasks: this volume consists of more than one device!\n");
	
	loadSBChunks(!noDump);

	if (!noDump) parseChunkTree(CTOP_DUMP_TREE);
	parseChunkTree(CTOP_LOAD);
	
	if (!noDump) parseRootTree(RTOP_DUMP_TREE, NULL, NULL);
	parseRootTree(RTOP_LOAD, NULL, NULL);

	if (!noDump)
	{
		parseFSTree(OBJID_FS_TREE, FSOP_DUMP_TREE, NULL, NULL, NULL, NULL, NULL);

		/* dump FS subtrees */
		size_t size = rootTree.size();
		for (size_t i = 0; i < size; i++)
		{
			KeyedItem& kItem = rootTree.at(i);

			/* this old code only dumped root-level (i.e. non-nested) subvolumes */
			/*if (kItem.key.type == TYPE_ROOT_REF && endian64(kItem.key.objectID) == OBJID_FS_TREE)*/
			if (kItem.key.type == TYPE_ROOT_REF && endian64(kItem.key.offset) >= 0x100 &&
				endian64(kItem.key.offset) < OBJID_MULTIPLE)
				parseFSTree((BtrfsObjID)endian64(kItem.key.offset), FSOP_DUMP_TREE,
					NULL, NULL, NULL, NULL, NULL);
		}
	}

	if (!useSubvolID && !useSubvolName)
	{
		int result;
		if ((result = parseRootTree(RTOP_DEFAULT_SUBVOL, NULL, NULL)) != 0)
		{
			printf("firstTasks: could not find the default subvolume!\n");
			cleanUp();
			exit(1);
		}
	}
	else if (useSubvolName)
	{
		if (strcmp(subvolName, "default") == 0)
			mountedSubvol = OBJID_FS_TREE;
		else
		{
			int result;
			if ((result = parseRootTree(RTOP_GET_SUBVOL_ID, subvolName, &mountedSubvol)) != 0)
			{
				printf("firstTasks: could not find the subvolume named '%s'!\n", subvolName);
				cleanUp();
				exit(1);
			}
		}
	}
	else
	{
		bool subvolExists;
		
		/* we can be fairly certain that these constraints will always hold */
		/* not enforcing these would allow the user to mount non-FS-type trees, which is definitely bad */
		if ((subvolID > (BtrfsObjID)0 && subvolID < (BtrfsObjID)0x100) ||
			(subvolID > (BtrfsObjID)-0x100 && subvolID < (BtrfsObjID)-1))
		{
			printf("firstTasks: %I64u is an impossible subvolume ID!\n", (unsigned __int64)subvolID);
			cleanUp();
			exit(1);
		}
		
		parseRootTree(RTOP_SUBVOL_EXISTS, &subvolID, &subvolExists);

		if (!subvolExists)
		{
			printf("firstTasks: could not find the subvolume with ID %I64u!\n", (unsigned __int64)subvolID);
			cleanUp();
			exit(1);
		}

		mountedSubvol = (subvolID == (BtrfsObjID)0 ? OBJID_FS_TREE : subvolID);
	}
}

void dokanError(int dokanResult)
{
	switch (dokanResult)
	{
	case DOKAN_SUCCESS:
		printf("Dokan terminated successfully.\n");
		break;
	case DOKAN_ERROR:
		printf("Dokan reported a general error!\n");
		exit(1);
	case DOKAN_DRIVE_LETTER_ERROR:
		printf("Dokan reported a bad drive letter!\n");
		exit(1);
	case DOKAN_DRIVER_INSTALL_ERROR:
		printf("Dokan reported it couldn't install the driver!\n");
		exit(1);
	case DOKAN_START_ERROR:
		printf("Dokan reported something is wrong with the driver!\n");
		exit(1);
	case DOKAN_MOUNT_ERROR:
		printf("Dokan reported it couldn't assign a drive letter or mount point!\n");
		exit(1);
	case DOKAN_MOUNT_POINT_ERROR:
		printf("Dokan reported the mount point is invalid!\n");
		exit(1);
	default:
		printf("Dokan returned an unknown error!\n");
		exit(1);
	}
}

void usage()
{
	printf("Usage: WinBtrfsCLI.exe <device> <mount point> [options]\n\n"
		"For the device argument, try something like \\Device\\HarddiskX\\PartitionY.\n"
		"Disks are indexed from zero; partitions are indexed from one.\n"
		"Example: /dev/sda1 = \\\\.\\Harddisk0Partition1\n\n"
		"You can also specify an image file to mount.\n\n"
		"The mount point can be a drive letter or an empty NTFS directory.\n\n"
		"Options:\n"
		"--no-dump               don't dump trees at startup\n"
		"--subvol=<subvol name>  mount the subvolume with the given name\n"
		"--subvol-id=<object ID> mount the subvolume with the given object ID\n");

	exit(1);
}

void unitTests()
{
	/* verify sizes of important on-disk structures */
	assert(sizeof(BtrfsTime) == 0x0c);
	assert(sizeof(BtrfsHeader) == 0x65);
	assert(sizeof(BtrfsDiskKey) == 0x11);
	assert(sizeof(BtrfsKeyPtr) == 0x21);
	assert(sizeof(BtrfsItem) == 0x19);
	assert(sizeof(BtrfsInodeItem) == 0xa0);
	assert(sizeof(BtrfsInodeRef) == 0x0a);
	assert(sizeof(BtrfsDirItem) == 0x1e);
	assert(sizeof(BtrfsDirIndex) == 0x1e);
	assert(sizeof(BtrfsExtentData) == 0x15);
	assert(sizeof(BtrfsExtentDataNonInline) == 0x20);
	assert(sizeof(BtrfsRootItem) == 0xef);
	assert(sizeof(BtrfsRootBackref) == 0x12);
	assert(sizeof(BtrfsRootRef) == 0x12);
	assert(sizeof(BtrfsBlockGroupItem) == 0x18);
	assert(sizeof(BtrfsChunkItem) == 0x30);
	assert(sizeof(BtrfsChunkItemStripe) == 0x20);
	assert(sizeof(BtrfsDevItem) == 0x62);
	assert(sizeof(BtrfsSuperblock) == 0x1000);

	/* ensure that enums are sized properly */
	assert(sizeof(BtrfsObjID) == sizeof(unsigned __int64));
}

int main(int argc, char **argv)
{
	PDOKAN_OPTIONS dokanOptions;
	int dokanResult;

	printf("WinBtrfs Command Line Interface\nCopyright (c) 2011 Justin Gottula\n\n");

	unitTests();

	if (argc < 3)
		usage();

	/* Need argument validity checking, MAX_PATH checking for buffer overruns */
	
	mbstowcs_s(NULL, devicePath, MAX_PATH, argv[1], strlen(argv[1]));
	mbstowcs_s(NULL, mountPoint, MAX_PATH, argv[2], strlen(argv[2]));

	for (int i = 3; i < argc; i++)
	{
		if (strcmp(argv[i], "--no-dump") == 0)
			noDump = true;
		else if (strncmp(argv[i], "--subvol-id=", 12) == 0)
		{
			if (strlen(argv[i]) > 12)
			{
				if (!useSubvolID && !useSubvolName)
				{
					if (sscanf(argv[i] + 12, "%I64u ", &subvolID) == 1)
						useSubvolID = true;
					else
					{
						printf("You entered an indecipherable subvolume object ID!\n\n");
						usage();
					}
				}
				else
				{
					printf("You specified more than one subvolume to mount!\n\n");
					usage();
				}
			}
			else
			{
				printf("You didn't specify a subvolume object ID!\n\n");
				usage();
			}
		}
		else if (strncmp(argv[i], "--subvol=", 9) == 0)
		{
			if (strlen(argv[i]) > 9)
			{
				if (!useSubvolID && !useSubvolName)
				{
					subvolName = argv[i] + 9;
					useSubvolName = true;
				}
				else
				{
					printf("You specified more than one subvolume to mount!\n\n");
					usage();
				}
			}
			else
			{
				printf("You didn't specify a subvolume name!\n\n");
				usage();
			}
		}
		else
		{
			printf("'%s' is not a recognized command-line option!\n\n", argv[i]);
			usage();
		}
	}

	dokanOptions = (PDOKAN_OPTIONS)malloc(sizeof(DOKAN_OPTIONS));
	dokanOptions->Version = 600;
	dokanOptions->ThreadCount = 1;			// eventually set this to zero or a user-definable count
	dokanOptions->Options = 0;				// look into this later
	dokanOptions->GlobalContext = 0;		// use this later if necessary
	dokanOptions->MountPoint = mountPoint;

	firstTasks();

	dokanResult = DokanMain(dokanOptions, &btrfsOperations);

	/* if DokanMain fails the first time complaining of a mount error,
		it will sometimes fix itself the second time (presumably by
		unmounting whatever was using the mount point in question) */
	if (dokanResult == DOKAN_MOUNT_ERROR)
		dokanResult = DokanMain(dokanOptions, &btrfsOperations);

	cleanUp();
	dokanError(dokanResult);
	
	return 0;
}
