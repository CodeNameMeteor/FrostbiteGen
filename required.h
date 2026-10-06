#ifndef REQUIRED_H
#define REQUIRED_H

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <Windows.h>
#include <vector>
#include <map>
#include <string>
#include <fstream>
#include <iostream>
#include <functional>
#include <algorithm>
#include <ctime>
#include <cstdint>
#include <cstring>
#include <cstdarg>
#include <cmath>
#include <limits>
#include <sstream>
#include <set>
#include <cctype>

extern char g_szBaseDir[MAX_PATH];

void Log(const char* szText, ...);
void CloseLog();
void GetDirFile(const char* file, char* out, size_t len);

#endif
