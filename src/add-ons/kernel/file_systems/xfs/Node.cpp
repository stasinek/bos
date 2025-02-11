/*
 * Copyright 2020, Shubham Bhagat, shubhambhagat111@yahoo.com
 * All rights reserved. Distributed under the terms of the MIT License.
 */


#include "Node.h"


NodeDirectory::NodeDirectory(Inode* inode)
	:
	fInode(inode),
	fDataMap(NULL),
	fLeafMap(NULL),
	fOffset(0),
	fDataBuffer(NULL),
	fLeafBuffer(NULL),
	fCurBlockNumber(-1)
{
}


NodeDirectory::~NodeDirectory()
{
	delete fDataMap;
	delete fLeafMap;
	delete fDataBuffer;
	delete fLeafBuffer;
}


status_t
NodeDirectory::Init()
{
	fLeafMap = new(std::nothrow) ExtentMapEntry;
	if (fLeafMap == NULL)
		return B_NO_MEMORY;

	fDataMap = new(std::nothrow) ExtentMapEntry;
	if (fDataMap == NULL)
		return B_NO_MEMORY;

	FillMapEntry(fInode->DataExtentsCount() - 3, fLeafMap);
	fCurLeafMapNumber = 1;
	FillMapEntry(0, fDataMap);
	return B_OK;
}


bool
NodeDirectory::IsNodeType()
{
	if (fCurLeafMapNumber != 1) {
		FillMapEntry(fInode->DataExtentsCount() - 3, fLeafMap);
		fCurLeafMapNumber = 1;
	}
	return fLeafMap->br_startoff
		== LEAF_STARTOFFSET(fInode->GetVolume()->BlockLog());
}


void
NodeDirectory::FillMapEntry(int num, ExtentMapEntry* fMap)
{
	void* directoryFork = DIR_DFORK_PTR(fInode->Buffer(), fInode->CoreInodeSize());
	void* pointerToMap = (void*)((char*)directoryFork + num * EXTENT_SIZE);
	uint64 firstHalf = *((uint64*)pointerToMap);
	uint64 secondHalf = *((uint64*)pointerToMap + 1);
		//dividing the 128 bits into 2 parts.

	firstHalf = B_BENDIAN_TO_HOST_INT64(firstHalf);
	secondHalf = B_BENDIAN_TO_HOST_INT64(secondHalf);
	fMap->br_state = firstHalf >> 63;
	fMap->br_startoff = (firstHalf & MASK(63)) >> 9;
	fMap->br_startblock = ((firstHalf & MASK(9)) << 43) | (secondHalf >> 21);
	fMap->br_blockcount = secondHalf & MASK(21);
	TRACE("Extent::Init: startoff:(%" B_PRIu64 "), startblock:(%" B_PRIu64 "),"
		"blockcount:(%" B_PRIu64 "),state:(%" B_PRIu8 ")\n", fMap->br_startoff, fMap->br_startblock,
		fMap->br_blockcount, fMap->br_state);
}


status_t
NodeDirectory::FillBuffer(int type, char* blockBuffer, int howManyBlocksFurthur)
{
	TRACE("FILLBUFFER\n");
	ExtentMapEntry* map;
	if (type == DATA)
		map = fDataMap;
	else if (type == LEAF)
		map = fLeafMap;
	else
		return B_BAD_VALUE;

	if (map->br_state != 0)
		return B_BAD_VALUE;

	ssize_t len = fInode->DirBlockSize();
	if (blockBuffer == NULL) {
		blockBuffer = new(std::nothrow) char[len];
		if (blockBuffer == NULL)
			return B_NO_MEMORY;
	}

	xfs_daddr_t readPos = fInode->FileSystemBlockToAddr(map->br_startblock
		+ howManyBlocksFurthur);

	if (read_pos(fInode->GetVolume()->Device(), readPos, blockBuffer, len)
		!= len) {
		ERROR("NodeDirectory::FillBlockBuffer(): IO Error");
		return B_IO_ERROR;
	}

	if (type == DATA) {
		fDataBuffer = blockBuffer;
		ExtentDataHeader* header = CreateDataHeader(fInode, fDataBuffer);
		if(header == NULL)
			return B_NO_MEMORY;
		if (header->Magic() == V4_DATA_HEADER_MAGIC) {
			TRACE("DATA BLOCK VALID\n");
			delete header;
		} else {
			TRACE("DATA BLOCK INVALID\n");
			delete header;
			return B_BAD_VALUE;
		}
	} else if (type == LEAF) {
		fLeafBuffer = blockBuffer;
		//TODO : see if we can do something with this header
		//ExtentLeafHeader* header = (ExtentLeafHeader*) fLeafBuffer;
	}

	return B_OK;
}


uint32
NodeDirectory::GetOffsetFromAddress(uint32 address)
{
	address = address * 8;
		// block offset in eight bytes, hence multiple with 8
	return address & (fInode->DirBlockSize() - 1);
}


uint32
NodeDirectory::FindHashInNode(uint32 hashVal)
{
	NodeHeader* header = (NodeHeader*)(void*)(fLeafBuffer);
	NodeEntry* entry = (NodeEntry*)(void*)(fLeafBuffer + sizeof(NodeHeader));
	int count = B_BENDIAN_TO_HOST_INT16(header->count);
	if ((NodeEntry*)(void*)fLeafBuffer + fInode->DirBlockSize()
		< &entry[count]) {
			return B_BAD_VALUE;
	}

	for (int i = 0; i < count; i++) {
		if (hashVal <= B_BENDIAN_TO_HOST_INT32(entry[i].hashval))
			return B_BENDIAN_TO_HOST_INT32(entry[i].before);
	}

	return 1;
}


int
NodeDirectory::EntrySize(int len) const
{
	int entrySize = sizeof(xfs_ino_t) + sizeof(uint8) + len + sizeof(uint16);
			// uint16 is for the tag
	if (fInode->HasFileTypeField())
		entrySize += sizeof(uint8);

	return ROUNDUP(entrySize, 8);
			// rounding off to closest multiple of 8
}


void
NodeDirectory::SearchAndFillDataMap(uint64 blockNo)
{
	int len = fInode->DataExtentsCount();

	for (int i = 0; i < len - 3; i++) {
		FillMapEntry(i, fDataMap);
		if (fDataMap->br_startoff <= blockNo
			&& (blockNo <= fDataMap->br_startoff + fDataMap->br_blockcount - 1))
			// Map found
			return;
	}
}


status_t
NodeDirectory::GetNext(char* name, size_t* length, xfs_ino_t* ino)
{
	TRACE("NodeDirectory::GetNext\n");
	status_t status;

	if (fDataBuffer == NULL) {
		status = FillBuffer(DATA, fDataBuffer, 0);
		if (status != B_OK)
			return status;
	}

	Volume* volume = fInode->GetVolume();

	void* entry; // This could be unused entry so we should check

	entry = (void*)(fDataBuffer + SizeOfDataHeader(fInode));

	uint32 blockNoFromAddress = BLOCKNO_FROM_ADDRESS(fOffset, volume);
	if (fOffset != 0 && blockNoFromAddress == fCurBlockNumber) {
		entry = (void*)(fDataBuffer
			+ BLOCKOFFSET_FROM_ADDRESS(fOffset, fInode));
		// This gets us a little faster to the next entry
	}

	uint32 curDirectorySize = fInode->Size();

	while (fOffset != curDirectorySize) {
		blockNoFromAddress = BLOCKNO_FROM_ADDRESS(fOffset, volume);

		TRACE("fOffset:(%" B_PRIu32 "), blockNoFromAddress:(%" B_PRIu32 ")\n",
			fOffset, blockNoFromAddress);
		if (fCurBlockNumber != blockNoFromAddress
			&& blockNoFromAddress > fDataMap->br_startoff
			&& blockNoFromAddress
				<= fDataMap->br_startoff + fDataMap->br_blockcount - 1) {
			// When the block is mapped in the same data
			// map entry but is not the first block
			status = FillBuffer(DATA, fDataBuffer,
				blockNoFromAddress - fDataMap->br_startoff);
			if (status != B_OK)
				return status;
			entry = (void*)(fDataBuffer + SizeOfDataHeader(fInode));
			fOffset = fOffset + SizeOfDataHeader(fInode);
			fCurBlockNumber = blockNoFromAddress;
		} else if (fCurBlockNumber != blockNoFromAddress) {
			// When the block isn't mapped in the current data map entry
			SearchAndFillDataMap(blockNoFromAddress);
			status = FillBuffer(DATA, fDataBuffer,
				blockNoFromAddress - fDataMap->br_startoff);
			if (status != B_OK)
				return status;
			entry = (void*)(fDataBuffer + SizeOfDataHeader(fInode));
			fOffset = fOffset + SizeOfDataHeader(fInode);
			fCurBlockNumber = blockNoFromAddress;
		}

		ExtentUnusedEntry* unusedEntry = (ExtentUnusedEntry*)entry;

		if (B_BENDIAN_TO_HOST_INT16(unusedEntry->freetag) == DIR2_FREE_TAG) {
			TRACE("Unused entry found\n");
			fOffset = fOffset + B_BENDIAN_TO_HOST_INT16(unusedEntry->length);
			entry = (void*)
				((char*)entry + B_BENDIAN_TO_HOST_INT16(unusedEntry->length));
			continue;
		}
		ExtentDataEntry* dataEntry = (ExtentDataEntry*) entry;

		uint16 currentOffset = (char*)dataEntry - fDataBuffer;
		TRACE("GetNext: fOffset:(%" B_PRIu32 "), currentOffset:(%" B_PRIu16 ")\n",
			BLOCKOFFSET_FROM_ADDRESS(fOffset, fInode), currentOffset);

		if (BLOCKOFFSET_FROM_ADDRESS(fOffset, fInode) > currentOffset) {
			entry = (void*)((char*)entry + EntrySize(dataEntry->namelen));
			continue;
		}

		if ((size_t)(dataEntry->namelen) >= *length)
			return B_BUFFER_OVERFLOW;

		fOffset = fOffset + EntrySize(dataEntry->namelen);
		memcpy(name, dataEntry->name, dataEntry->namelen);
		name[dataEntry->namelen] = '\0';
		*length = dataEntry->namelen + 1;
		*ino = B_BENDIAN_TO_HOST_INT64(dataEntry->inumber);

		TRACE("Entry found. Name: (%s), Length: (%" B_PRIuSIZE "),ino: (%" B_PRIu64 ")\n",
			name,*length, *ino);
		return B_OK;
	}

	return B_ENTRY_NOT_FOUND;
}


status_t
NodeDirectory::Lookup(const char* name, size_t length, xfs_ino_t* ino)
{
	TRACE("NodeDirectory: Lookup\n");
	TRACE("Name: %s\n", name);
	uint32 hashValueOfRequest = hashfunction(name, length);
	TRACE("Hashval:(%" B_PRIu32 ")\n", hashValueOfRequest);

	status_t status;
	if (fCurLeafBufferNumber != 1) {
		if (fCurLeafMapNumber != 1) {
			FillMapEntry(fInode->DataExtentsCount() - 3, fLeafMap);
			fCurLeafMapNumber = 1;
		}
		status = FillBuffer(LEAF, fLeafBuffer, 0);
		if (status != B_OK)
			return status;
		fCurLeafBufferNumber = 1;
	}
	/* Leaf now has the nodes. */
	uint32 rightMapOffset = FindHashInNode(hashValueOfRequest);
	if (rightMapOffset == 1){
		TRACE("Not in this directory.\n");
		return B_ENTRY_NOT_FOUND;
	}

	TRACE("rightMapOffset:(%" B_PRIu32 ")\n", rightMapOffset);

	FillMapEntry(fInode->DataExtentsCount() - 2, fLeafMap);
	fCurLeafMapNumber = 2;
	status = FillBuffer(LEAF, fLeafBuffer,
		rightMapOffset - fLeafMap->br_startoff);
	if (status != B_OK)
		return status;
	fCurLeafBufferNumber = 2;

	ExtentLeafHeader* leafHeader = CreateLeafHeader(fInode, fLeafBuffer);
	if(leafHeader == NULL)
		return B_NO_MEMORY;
	ExtentLeafEntry* leafEntry =
		(ExtentLeafEntry*)(void*)(fLeafBuffer + SizeOfLeafHeader(fInode));
	if (leafEntry == NULL)
		return B_NO_MEMORY;

	int numberOfLeafEntries = leafHeader->Count();
	TRACE("numberOfLeafEntries:(%" B_PRId32 ")\n", numberOfLeafEntries);
	int left = 0;
	int mid;
	int right = numberOfLeafEntries - 1;
	Volume* volume = fInode->GetVolume();

	/*
	* Trying to find the lowerbound of hashValueOfRequest
	* This is slightly different from bsearch(), as we want the first
	* instance of hashValueOfRequest and not any instance.
	*/
	while (left < right) {
		mid = (left + right) / 2;
		uint32 hashval = B_BENDIAN_TO_HOST_INT32(leafEntry[mid].hashval);
		if (hashval >= hashValueOfRequest) {
			right = mid;
			continue;
		}
		if (hashval < hashValueOfRequest) {
			left = mid + 1;
		}
	}
	TRACE("left:(%" B_PRId32 "), right:(%" B_PRId32 ")\n", left, right);

	while (B_BENDIAN_TO_HOST_INT32(leafEntry[left].hashval)
			== hashValueOfRequest) {

		uint32 address = B_BENDIAN_TO_HOST_INT32(leafEntry[left].address);
		if (address == 0) {
			left++;
			continue;
		}

		uint32 dataBlockNumber = BLOCKNO_FROM_ADDRESS(address * 8, volume);
		uint32 offset = BLOCKOFFSET_FROM_ADDRESS(address * 8, fInode);

		TRACE("DataBlockNumber:(%" B_PRIu32 "), offset:(%" B_PRIu32 ")\n",
			dataBlockNumber, offset);
		if (dataBlockNumber != fCurBlockNumber) {
			fCurBlockNumber = dataBlockNumber;
			SearchAndFillDataMap(dataBlockNumber);
			status = FillBuffer(DATA, fDataBuffer,
				dataBlockNumber - fDataMap->br_startoff);
			if (status != B_OK)
				return B_OK;
		}

		TRACE("offset:(%" B_PRIu32 ")\n", offset);
		ExtentDataEntry* entry = (ExtentDataEntry*)(fDataBuffer + offset);

		int retVal = strncmp(name, (char*)entry->name, entry->namelen);
		if (retVal == 0) {
			*ino = B_BENDIAN_TO_HOST_INT64(entry->inumber);
			TRACE("ino:(%" B_PRIu64 ")\n", *ino);
			return B_OK;
		}
		left++;
	}

	delete leafHeader;
	return B_ENTRY_NOT_FOUND;
}
