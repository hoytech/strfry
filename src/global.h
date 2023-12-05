#pragma once

#include <parallel_hashmap/phmap.h>
#include <parallel_hashmap/btree.h>

using namespace phmap;


#include "constants.h"


std::string renderIP(std::string_view ipBytes);
std::string renderSize(uint64_t si);
std::string renderPercent(double p);
uint64_t parseUint64(const std::string &s);
std::string parseIP(const std::string &ip);
uint64_t getDBVersion(lmdb::txn &txn);
void exitOnSigPipe();

extern lmdb::dbi negentropyDbi;
