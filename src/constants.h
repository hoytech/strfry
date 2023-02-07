#pragma once

const uint64_t CURR_DB_VERSION = 1;
const size_t MAX_SUBID_SIZE = 71; // Statically allocated size in SubId
const uint64_t MAX_TIMESTAMP = 17179869184; // Safety limit to ensure it can fit in quadrable key. Good until year 2514.
