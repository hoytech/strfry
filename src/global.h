#pragma once

#include <parallel_hashmap/phmap.h>
#include <parallel_hashmap/btree.h>

using namespace phmap;


#include <quadrable.h>

quadrable::Quadrable getQdbInstance(lmdb::txn &txn);
quadrable::Quadrable getQdbInstance();


std::string renderIP(std::string_view ipBytes);


#include "constants.h"
