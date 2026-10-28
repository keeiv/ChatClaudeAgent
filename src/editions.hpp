// Product editions: the ordinary CCA and the special editions built from the same sources.
#pragma once

#include <string>

// Ordinary builds return false; a special edition is a separate product line with its own version numbers.
bool specialEdition();
const char* editionId();  // "" for the ordinary edition
const char* editionName();
