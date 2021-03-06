/************************************************
*
* Author: Jan Newger
* Date: 6.28.2008
*
************************************************/

#include <boost/foreach.hpp>
#include "resource.h"

#include "IDAStealthWTLWrapper.h"
#include "IDAStealth.h"
#include <HideDebugger/ntdll.h>
#include <NCodeHook/NCodeHookInstantiation.h>
#include <HideDebugger/HideDebuggerConfig.h>
#include "ResourceItem.h"
#include "LocalStealthSession.h"
#include <Windows.h>

void logCallback(const std::string& str);
ResourceItem getDriverResource();
std::string getStealthDll();
void showLogGUI();

namespace
{
	IDAStealth::LocalStealthSession session_;
	NCodeHookIA32 nCodeHook_;

	DbgUiConvertStateChangeStructureFPtr origDbgUiConvStateChngStruct = NULL;
	DebugActiveProcessFPtr origDebugActiveProcess = NULL;
}

/*********************************************************************
* Function: init
*
* init is a plugin_t function. It is executed when the plugin is
* initially loaded by IDA.
* Three return codes are possible:
*    PLUGIN_SKIP - Plugin is unloaded and not made available
*    PLUGIN_KEEP - Plugin is kept in memory
*    PLUGIN_OK   - Plugin will be loaded upon 1st use
*
* Check are added here to ensure the plug-in is compatible with
* the current disassembly.
*********************************************************************/
int __stdcall init()
{
	if (inf.filetype != f_PE || !inf.is_32bit()) return PLUGIN_SKIP;
	if (!hook_to_notification_point(HT_DBG, callback, NULL))
	{
		msg("IDAStealth: Could not hook to notification point\n");
		return PLUGIN_SKIP;
	}
	try
	{
		localStealth();
	}
	catch (const std::exception& e)
	{
		msg("IDAStealth: Error while trying to apply local stealth: %s\n", e.what());
		return PLUGIN_SKIP;
	}
	
	return PLUGIN_KEEP;
}

/*********************************************************************
* Function: term
*
* term is a plugin_t function. It is executed when the plugin is
* unloading. Typically cleanup code is executed here.
*********************************************************************/
void __stdcall term()
{
	unhook_from_notification_point(HT_DBG, callback, NULL);
}

/*********************************************************************
* Function: run
*
* run is a plugin_t function. It is executed when the plugin is run.
*
* The argument 'arg' can be passed by adding an entry in
* plugins.cfg or passed manually via IDC:
*
*   success RunPlugin(string name, long arg);
*********************************************************************/
void __stdcall run(int arg)
{

	//  Uncomment the following code to allow plugin unloading.
	//  This allows the editing/building of the plugin without
	//  restarting IDA.
	//
	//  1. to unload the plugin execute the following IDC statement:
	//        RunPlugin("IDAStealth", 666);
	//  2. Make changes to source code and rebuild within Visual Studio
	//  3. Copy plugin to IDA plugin dir
	//     (may be automatic if option was selected within wizard)
	//  4. Run plugin via the menu, hotkey, or IDC statement
	//
	if (arg == 666)
	{
		PLUGIN.flags |= PLUGIN_UNL;
		msg("Unloading IDAStealth plugin...\n");
	}
	else
	{
		IDAStealthWTLWrapper& wtlWrapper = IDAStealthWTLWrapper::getInstance();
		wtlWrapper.showGUI((HWND)callui(ui_get_hwnd).vptr);
	}

	return;
}

// hooks local function from ntdll in order to prevent special handling of DBG_PRINTEXCEPTION_C
// by WaitForDebugEvent loop of IDAs debugger
NTSTATUS NTAPI DbgUiConvStateChngStructHook(PDBGUI_WAIT_STATE_CHANGE WaitStateChange, LPDEBUG_EVENT DebugEvent)
{
	__try
	{
		if (WaitStateChange->StateInfo.Exception.ExceptionRecord.ExceptionCode == DBG_PRINTEXCEPTION_C)
		{
			DebugEvent->dwProcessId = (DWORD)WaitStateChange->AppClientId.UniqueProcess;
			DebugEvent->dwThreadId = (DWORD)WaitStateChange->AppClientId.UniqueThread;
			DebugEvent->dwDebugEventCode = DbgReplyPending;
			return 0;
		}
	}
	__except(EXCEPTION_EXECUTE_HANDLER)
	{
		return 0;
	}
	
	return origDbgUiConvStateChngStruct(WaitStateChange, DebugEvent);
}

#define MakePtr( cast, ptr, addValue ) (cast)( (DWORD_PTR)(ptr) + (DWORD_PTR)(addValue) )

// we need to be able to write to the debuggee before IDA attaches so we hook the debug API itself
BOOL WINAPI DebugActiveProcessHook(DWORD dwProcessId)
{
	// we will replace the whole code section of ntdll.dll in the foreign process!
	HANDLE hNtDll = GetModuleHandle("ntdll.dll");
	if (hNtDll != INVALID_HANDLE_VALUE)
	{
		PIMAGE_DOS_HEADER pDosHeader = (PIMAGE_DOS_HEADER)hNtDll;
		PIMAGE_NT_HEADERS pNTHeader = MakePtr(PIMAGE_NT_HEADERS, pDosHeader, pDosHeader->e_lfanew);
		LPVOID base = (LPVOID)(pNTHeader->OptionalHeader.BaseOfCode + pNTHeader->OptionalHeader.ImageBase);
		
		HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, dwProcessId);
		if (hProcess)
		{
			DWORD oldProtect;
			DWORD codeSize = pNTHeader->OptionalHeader.SizeOfCode;
			if (VirtualProtectEx(hProcess, base, codeSize, PAGE_READWRITE, &oldProtect))
			{
				if (!WriteProcessMemory(hProcess, base, base, codeSize, NULL))
					msg("IDAStealth: Error while writing to process (KillAntiAttach)\n");
				VirtualProtectEx(hProcess, base, codeSize, oldProtect, NULL);
			}
			CloseHandle(hProcess);
		}
	}
	return origDebugActiveProcess(dwProcessId);
}

// add/remove stealth hooks to/from the *debugger process*!
// this is NOT carried out on the remote debugger side
void localStealth()
{
	HideDebuggerConfig& config = HideDebuggerConfig::getInstance();
	if (config.getDbgPrintException())
		origDbgUiConvStateChngStruct = nCodeHook_.createHookByName("ntdll.dll",
																  "DbgUiConvertStateChangeStructure",
																  DbgUiConvStateChngStructHook);
	else nCodeHook_.removeHook(DbgUiConvStateChngStructHook);
	if (config.getKillAntiAttach())
		origDebugActiveProcess = nCodeHook_.createHookByName("kernel32.dll",
															"DebugActiveProcess",
															DebugActiveProcessHook);
	else nCodeHook_.removeHook(DebugActiveProcessHook);
}

void setExceptionOptions(bool ignoreException)
{
	// get old settings first
	uint oldSettings = set_debugger_options(0) & ~(EXCDLG_ALWAYS | EXCDLG_UNKNOWN);
	uint newSettings = ignoreException ? oldSettings | EXCDLG_NEVER : oldSettings | EXCDLG_UNKNOWN;
	set_debugger_options(newSettings);
} 

void handleDebugException(const debug_event_t* dbgEvent)
{
	const HideDebuggerConfig& config = HideDebuggerConfig::getInstance();
	if (config.getPassExceptions())
	{
		// since the user could add new exceptions while debugging, we need to 
		// retrieve the whole list again for every event
		excvec_t* exceptions = retrieve_exceptions();
		bool ignoreException = true;
		BOOST_FOREACH(const exception_info_t& exInfo, *exceptions)
		{
			// is this a known exception code?
			if (dbgEvent->exc.code == (int)exInfo.code)
			{
				ignoreException = false;
				break;
			}
		}
		setExceptionOptions(ignoreException);
	}
}

int idaapi callback(void*, int notification_code, va_list va)
{
	try
	{
		const HideDebuggerConfig& config = HideDebuggerConfig::getInstance();
		switch (notification_code)
		{
		case dbg_process_attach:
			{
				const debug_event_t* dbgEvent = va_arg(va, const debug_event_t*);
				session_.handleDbgAttach(dbgEvent->pid, HideDebuggerConfig::getDefaultConfigFile(),
					config.getCurrentProfile());
			}
			break;

		case dbg_process_start:
			{
				const debug_event_t* dbgEvent = va_arg(va, const debug_event_t*);
				session_.handleProcessStart(dbgEvent->pid, dbgEvent->modinfo.base, 
				HideDebuggerConfig::getDefaultConfigFile(), config.getCurrentProfile());
			}
			break;

		case dbg_process_exit:
			va_arg(va, const debug_event_t*);
			session_.handleProcessExit();
			break;

		case dbg_bpt:
			{
				thid_t tid = va_arg(va, thid_t);
				ea_t breakpoint_ea = va_arg(va, ea_t);
				va_arg(va, int*);
				session_.handleBreakPoint(tid, breakpoint_ea);
			}
			break;

		case dbg_exception:
			{
				const debug_event_t* dbgEvent = va_arg(va, const debug_event_t*);
				va_arg(va, int*);
				handleDebugException(dbgEvent);
			}
			break;
		}
	}
	catch (const std::exception& e)
	{
		string txt = "IDAStealth: Error in IDA callback: " + std::string(e.what()) + "\n";
		msg(txt.c_str());
	}
	catch (...)
	{
		msg("IDAStealth: Unknown error (this should never happen!)\n");
	}
	return 0;
}

//////////////////////////////////////////////////////////////////////////

char comment[] 	= "Short one line description about the plugin";
char help[] 	= "My plugin:\n"
"\n"
"Multi-line\n"
"description\n";

/* Plugin name listed in (Edit | Plugins) */
char wanted_name[] 	= "IDAStealth";

/* plug-in hotkey */
char wanted_hotkey[] 	= "";

/* defines the plugins interface to IDA */
plugin_t PLUGIN =
{
	IDP_INTERFACE_VERSION,
	0,              // plugin flags
	init,           // initialize
	term,           // terminate. this pointer may be NULL.
	run,            // invoke plugin
	comment,        // comment about the plugin
	help,           // multiline help about the plugin
	wanted_name,    // the preferred short name of the plugin
	wanted_hotkey   // the preferred hotkey to run the plugin
};