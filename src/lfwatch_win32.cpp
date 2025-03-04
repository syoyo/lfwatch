#ifdef _WIN32

#include <iostream>
#include <cstring>
#include <cstdlib>
#include <string>
#include <map>
#include <vector>
#include <windows.h>
#include "lfwatch_win32.h"

namespace lfw {
//Callback for Win32 to tell us about file events
void CALLBACK watch_callback(DWORD err, DWORD num_bytes, LPOVERLAPPED overlapped);

//Log out the error message for an error code
std::string get_error_msg(DWORD err){
	LPSTR err_msg;
	size_t size = FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER
		| FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPSTR)&err_msg, 0, nullptr);
	std::string msg{err_msg, size};
	LocalFree(err_msg);
	return msg;
}
//We use our own enums so they can be or'd together but can correspond to overlapping
//Win32 FILE_NOTIFY or FILE_ACTION values, these functions remap the according
//file_action/notify enums to our enums and vice-versa
DWORD remap_file_notify(uint32_t mask){
	DWORD remap = 0;
	if (mask & Notify::FILE_MODIFIED){
		remap |= FILE_NOTIFY_CHANGE_LAST_WRITE;
	}
	if (mask & Notify::FILE_CREATED || mask & Notify::FILE_REMOVED
		|| mask & Notify::FILE_RENAMED_OLD_NAME || Notify::FILE_RENAMED_NEW_NAME)
	{
		remap |= FILE_NOTIFY_CHANGE_FILE_NAME;
	}
	return remap;
}
//The file action in the notify event only can be one value
uint32_t remap_file_action(DWORD action){
	switch (action){
		case FILE_ACTION_MODIFIED:
			return Notify::FILE_MODIFIED;
		case FILE_ACTION_ADDED:
			return Notify::FILE_CREATED;
		case FILE_ACTION_REMOVED:
			return Notify::FILE_REMOVED;
		case FILE_ACTION_RENAMED_OLD_NAME:
			return Notify::FILE_RENAMED_OLD_NAME;
		case FILE_ACTION_RENAMED_NEW_NAME:
			return Notify::FILE_RENAMED_NEW_NAME;
		default:
			return 0;
	}
}
//Register a watch to receive events
void register_watch(WatchData &watch){
	std::memset(&watch.info_buf[0], 0, watch.info_buf.size());
	bool status = ReadDirectoryChangesW(watch.dir_handle, &watch.info_buf[0],
		watch.info_buf.size(), false, remap_file_notify(watch.filter), nullptr,
		&watch.overlapped, watch_callback);
	if (!status){
		std::cerr << "lfw Error: could not register watch on " << watch.dir_name
			<< ": " << get_error_msg(GetLastError()) << std::endl;
	}
}
void emit_events(WatchData &watch){
	PFILE_NOTIFY_INFORMATION info;
	size_t offset = 0;
	do {
		info = reinterpret_cast<PFILE_NOTIFY_INFORMATION>(&watch.info_buf[0] + offset);
		offset += info->NextEntryOffset;
		//FileNameLength is size in bytes of the 16-bit Unicode string so, compute
		//the max number of chars that it could contain
		//This is done to put the null terminator on the end of the string since
		//Win32 doesn't null-terminate
		int n_chars = info->FileNameLength / 2;
		std::vector<wchar_t> wfname(n_chars + 1);
		std::memcpy(&wfname[0], info->FileName, info->FileNameLength);
		char fname[MAX_PATH + 1] = { 0 };
		std::wcstombs(fname, &wfname[0], MAX_PATH);
		//Since FILE_NOTIFY_CHANGE_FILE_NAME gives all create/delete/rename events it's possible that
		//we only want create but have gotten one of the other two, so make sure we actually want this
		uint32_t action = remap_file_action(info->Action);
		if (action & watch.filter){
			watch.callback(EventData{watch.dir_name, fname, watch.filter, action});
		}
	}
	while (info->NextEntryOffset != 0);
}
void CALLBACK watch_callback(DWORD err, DWORD num_bytes, LPOVERLAPPED overlapped){
	SetEvent(overlapped->hEvent);
	if (err == ERROR_SUCCESS){
		WatchData *watch = reinterpret_cast<WatchData*>(overlapped);
		emit_events(*watch);
		//Re-register to listen again
		register_watch(*watch);
		ResetEvent(overlapped->hEvent);
	}
	//If we're being cancelled it's not an error
	else if (err != ERROR_OPERATION_ABORTED){
		std::cerr << "lfw Error: watch callback error: " << get_error_msg(err) << std::endl;
	}
}
void cancel(WatchData &watch){
	if (CancelIo(watch.dir_handle) == 0){
		std::cerr << "lfw Error: cancelling watch: "
			<< get_error_msg(GetLastError()) << std::endl;
	}
	//Give the watcher time to get the cancellation event and set the event
	MsgWaitForMultipleObjectsEx(0, nullptr, 0, QS_ALLINPUT, MWMO_ALERTABLE);
	DWORD status = WaitForSingleObject(watch.overlapped.hEvent, INFINITE);
	if (status == WAIT_FAILED){
		std::cerr << "lfw Error: failed to wait for cancellation: "
			<< get_error_msg(GetLastError()) << std::endl;
	}
	CloseHandle(watch.dir_handle);
	CloseHandle(watch.overlapped.hEvent);
}

WatchData::WatchData(HANDLE handle, const std::string &dir, uint32_t filter, const Callback &cb)
	: dir_handle(handle), dir_name(dir), filter(filter), callback(cb)
{
	std::memset(&overlapped, 0, sizeof(overlapped));
	overlapped.hEvent = CreateEvent(nullptr, true, false, nullptr);
}

WatchWin32::WatchWin32(){}
WatchWin32::~WatchWin32(){
	for (auto &pair : watchers){
		cancel(pair.second);
	}
}
void WatchWin32::watch(const std::string &dir, uint32_t filters, const Callback &callback){
	auto fnd = watchers.find(dir);
	if (fnd != watchers.end() && fnd->second.filter != filters){
		fnd->second.filter = filters;
		fnd->second.callback = callback;
		register_watch(fnd->second);
		return;
	}
	HANDLE handle = CreateFile(dir.c_str(), FILE_LIST_DIRECTORY,
		FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
		nullptr);
	if (handle == INVALID_HANDLE_VALUE){
		std::cerr << "lfw Error: failed to create handle for " << dir
			<< ": " << get_error_msg(GetLastError()) << std::endl;
		return;
	}
	auto it = watchers.emplace(std::make_pair(dir, WatchData{handle, dir, filters, callback}));
	register_watch(it.first->second);
}
void WatchWin32::remove(const std::string &dir){
	auto fnd = watchers.find(dir);
	if (fnd != watchers.end()){
		cancel(fnd->second);
		watchers.erase(fnd);
	}
}
void WatchWin32::update(){
	MsgWaitForMultipleObjectsEx(0, nullptr, 0, QS_ALLINPUT, MWMO_ALERTABLE);
}
}

#endif

