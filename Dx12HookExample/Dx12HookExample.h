// The following ifdef block is the standard way of creating macros which make exporting
// from a DLL simpler. All files within this DLL are compiled with the DX12HOOKEXAMPLE_EXPORTS
// symbol defined on the command line. This symbol should not be defined on any project
// that uses this DLL. This way any other project whose source files include this file see
// DX12HOOKEXAMPLE_API functions as being imported from a DLL, whereas this DLL sees symbols
// defined with this macro as being exported.
#ifdef DX12HOOKEXAMPLE_EXPORTS
#define DX12HOOKEXAMPLE_API __declspec(dllexport)
#else
#define DX12HOOKEXAMPLE_API __declspec(dllimport)
#endif

// This class is exported from the dll
class DX12HOOKEXAMPLE_API CDx12HookExample {
public:
	CDx12HookExample(void);
	// TODO: add your methods here.
};

extern DX12HOOKEXAMPLE_API int nDx12HookExample;

DX12HOOKEXAMPLE_API int fnDx12HookExample(void);
