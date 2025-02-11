/*
 * Copyright 2022, Raghav Sharma, raghavself28@gmail.com
 * Copyright 2020, Shubham Bhagat, shubhambhagat111@yahoo.com
 * All rights reserved. Distributed under the terms of the MIT License.
 */
#ifndef _EXTENT_H_
#define _EXTENT_H_


#include "Inode.h"
#include "system_dependencies.h"


#define DIR2_BLOCK_HEADER_MAGIC 0x58443242
	// for v4 system
#define DIR3_BLOCK_HEADER_MAGIC 0x58444233
	// for v5 system
#define DIR2_FREE_TAG 0xffff
#define XFS_DIR2_DATA_FD_COUNT 3
#define EXTENT_REC_SIZE		128
#define EXTENT_SIZE 16
#define BLOCKNO_FROM_ADDRESS(n, volume) \
	((n) >> (volume->BlockLog() + volume->DirBlockLog()))
#define BLOCKOFFSET_FROM_ADDRESS(n, inode) ((n) & (inode->DirBlockSize() - 1))
#define LEAF_STARTOFFSET(n) 1UL << (35 - (n))


// xfs_exntst_t
enum ExtentState {
	XFS_EXT_NORM,
	XFS_EXT_UNWRITTEN,
	XFS_EXT_INVALID
};


// xfs_dir2_data_free_t
struct FreeRegion {
			uint16				offset;
			uint16				length;
};


// This class will act as interface for V4 and V5 data header
class ExtentDataHeader {
public:

			virtual			~ExtentDataHeader()		=	0;
			virtual uint32				Magic()		=	0;
			virtual uint64				Blockno()	=	0;
			virtual uint64				Lsn()		=	0;
			virtual uint64				Owner()		=	0;
			virtual uuid_t*				Uuid()		=	0;
};


//xfs_dir_data_hdr_t
class ExtentDataHeaderV4 : public ExtentDataHeader {
public :

								ExtentDataHeaderV4(const char* buffer);
								~ExtentDataHeaderV4();
			void				SwapEndian();
			uint32				Magic();
			uint64				Blockno();
			uint64				Lsn();
			uint64				Owner();
			uuid_t*				Uuid();

			uint32				magic;
private:
			FreeRegion			bestfree[XFS_DIR2_DATA_FD_COUNT];
};


// xfs_dir3_data_hdr_t
class ExtentDataHeaderV5 : public ExtentDataHeader {
public:
								ExtentDataHeaderV5(const char* buffer);
								~ExtentDataHeaderV5();
			void				SwapEndian();
			uint32				Magic();
			uint64				Blockno();
			uint64				Lsn();
			uint64				Owner();
			uuid_t*				Uuid();
public:
			uint32				magic;
			uint32				crc;
private:
			uint64				blkno;
			uint64				lsn;
			uuid_t				uuid;
			uint64				owner;
			FreeRegion			bestfree[XFS_DIR2_DATA_FD_COUNT];
			uint32				pad;
};

#define XFS_EXTENT_CRC_OFF  offsetof(ExtentDataHeaderV5, crc)
#define XFS_EXTENT_V5_VPTR_OFF offsetof(ExtentDataHeaderV5, magic)
#define XFS_EXTENT_V4_VPTR_OFF offsetof(ExtentDataHeaderV4, magic)


// xfs_dir2_data_entry_t
struct ExtentDataEntry {
			xfs_ino_t			inumber;
			uint8				namelen;
			uint8				name[];

// Followed by a file type (8bit) if applicable and a 16bit tag
// tag is the offset from start of the block
};


// xfs_dir2_data_unused_t
struct ExtentUnusedEntry {
			uint16				freetag;
				// takes the value 0xffff
			uint16				length;
				// freetag+length overrides the inumber of an entry
			uint16				tag;
};


// xfs_dir2_leaf_entry_t
struct ExtentLeafEntry {
			uint32				hashval;
			uint32				address;
				// offset into block after >> 3
};


// xfs_dir2_block_tail_t
struct ExtentBlockTail {
			uint32				count;
				// # of entries in leaf
			uint32				stale;
				// # of free leaf entries
};


class Extent
{
public:
								Extent(Inode* inode);
								~Extent();
			status_t			Init();
			bool				IsBlockType();
			bool				VerifyHeader(ExtentDataHeader* header);
			void				FillMapEntry(void* pointerToMap);
			status_t			FillBlockBuffer();
			ExtentBlockTail*	BlockTail();
			ExtentLeafEntry*	BlockFirstLeaf(ExtentBlockTail* tail);
			xfs_ino_t			GetIno();
			uint32				GetOffsetFromAddress(uint32 address);
			int					EntrySize(int len) const;
			status_t			GetNext(char* name, size_t* length,
									xfs_ino_t* ino);
			status_t			Lookup(const char* name, size_t length,
									xfs_ino_t* id);
private:
			Inode*				fInode;
			ExtentMapEntry*		fMap;
			uint32				fOffset;
			char*				fBlockBuffer;
				// This isn't inode data. It holds the directory block.
};


ExtentDataHeader*
CreateDataHeader(Inode* inode, const char* buffer);


uint32
SizeOfDataHeader(Inode* inode);

#endif
