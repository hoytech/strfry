#pragma once

#include <parallel_hashmap/phmap.h>
#include <parallel_hashmap/btree.h>

using namespace phmap;


#include "constants.h"
#include "Bytes32.h"


Bytes32 sha256(std::string_view inp);
std::string renderIP(std::string_view ipBytes);
std::string renderSize(uint64_t si);
std::string renderPercent(double p);
uint64_t parseUint64(const std::string &s);
std::string parseIP(const std::string &ip);
uint64_t getDBVersion(lmdb::txn &txn);
void exitOnSigPipe();
void setNonBlocking(int fd);
void writeWithTimeout(int fd, std::string_view buf, uint64_t timeoutMilliseconds);
std::string readLineWithTimeout(std::string &buf, int fd, uint64_t timeoutMilliseconds, size_t maxLineSize);

extern lmdb::dbi negentropyDbi;
