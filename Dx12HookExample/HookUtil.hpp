#pragma once
#include <map>
#include <string>
#include <Windows.h>

namespace
{
	struct HookInfo
	{
		std::string name = "";
		__int64 address = 0;
		SIZE_T index = 0;
		LPVOID* original = 0;
	};

	std::map<std::string, HookInfo> hookMap;

	static PBYTE WINAPI WriteToVTable(PDWORD64* ppVTable, PVOID hook, SIZE_T iIndex)
	{
		DWORD dwOld = 0;
		VirtualProtect((void*)((*ppVTable) + iIndex), sizeof(PDWORD64), PAGE_EXECUTE_READWRITE, &dwOld);

		PBYTE pOrig = ((PBYTE)(*ppVTable)[iIndex]);
		(*ppVTable)[iIndex] = (DWORD64)hook;

		VirtualProtect((void*)((*ppVTable) + iIndex), sizeof(PDWORD64), dwOld, &dwOld);
		return pOrig;
	}
}

static void CreateVTableHook(const std::string& name, PDWORD64* ppVtable, PVOID hook, SIZE_T index, PVOID original)
{
	LPVOID* pOriginal = reinterpret_cast<LPVOID*>(original);
	*pOriginal = reinterpret_cast<LPVOID>(WriteToVTable(ppVtable, hook, index));

	HookInfo info{};
	info.name = name;
	info.address = reinterpret_cast<__int64>(ppVtable);
	info.original = pOriginal;
	info.index = index;

	hookMap.emplace(name, info);
}

static void RemoveHook(const std::string& name)
{
	auto result = hookMap.find(name);
	if (result == hookMap.end())
	{
		printf("Could not find hook %s to disable\n", name.c_str());
		return;
	}

	const HookInfo& info = result->second;
	*info.original = WriteToVTable((PDWORD64*)info.address, *info.original, info.index);
}