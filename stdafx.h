// stdafx.h : include file for standard system include files,
// or project specific include files that are used frequently, but
// are changed infrequently
//

#pragma once

#define WIN32_LEAN_AND_MEAN             // Exclude rarely-used stuff from Windows headers
#define PTCHAR TCHAR*

#include <tchar.h>
#include <stdio.h>
#include <process.h>

// Platform SDK
#include <d3d9.h>
#include <dshow.h>
#include <dvdmedia.h>
#include <initguid.h>

// Intel Media SDK
#include <mfxvideo++.h>

// STL
#include <list>
#include <vector>
#include <deque>
#include <set>
#include <algorithm>

// use our own ASSERT macro
#undef ASSERT
