#include <windows.h>
#include <iostream>
#include <string>

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2) return 2;
    std::wstring directory = argv[1];
    std::wstring path = directory + L"\\SonyMTPCamera.dll";
    HMODULE module = LoadLibraryExW(path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module) {
        std::cerr << "Native DLL load failed: " << GetLastError() << '\n';
        return 1;
    }
    const char* exports[] = {
        "GetDeviceCount", "OpenDevice", "OpenDeviceEx", "CloseDevice", "GetDeviceInfo",
        "GetPreviewImage", "StartCapture", "GetCaptureStatus", "CancelCapture", "GetCameraInfo",
        "GetPortableDeviceCount", "GetPortableDeviceInfo", "GetSupportedDeviceCount", "GetSupportedDeviceInfo",
        "GetPropertyList", "GetPropertyDescriptor", "GetPropertyValueOption", "GetSinglePropertyValue",
        "GetAllPropertyValues", "SetPropertyValue", "SetExposureTime", "SetFocusPosition", "GetFocusPosition",
        "GetFocusLimit", "GetLensCount", "GetLensInfo", "SetAttachedLens", "TestFunc"
    };
    for (const char* name : exports) {
        if (!GetProcAddress(module, name)) {
            std::cerr << "Missing native export: " << name << '\n';
            FreeLibrary(module);
            return 1;
        }
    }
    for (const wchar_t* dependency : { L"libraw.dll", L"turbojpeg.dll" }) {
        wchar_t actual[32768] = {};
        HMODULE loaded = GetModuleHandleW(dependency);
        std::wstring expected = directory + L"\\" + dependency;
        if (!loaded || !GetModuleFileNameW(loaded, actual, 32768) || _wcsicmp(actual, expected.c_str()) != 0) {
            std::wcerr << L"Unexpected dependency location for " << dependency << L": " << actual << L'\n';
            FreeLibrary(module);
            return 1;
        }
    }
    auto countLenses = reinterpret_cast<DWORD(__cdecl*)()>(GetProcAddress(module, "GetLensCount"));
    DWORD lenses = countLenses(); // Registry/resource path only; no camera enumeration.
    if (lenses == 0) {
        std::cerr << "No supported lens definitions were loaded.\n";
        FreeLibrary(module);
        return 1;
    }
    FreeLibrary(module);
    std::cout << "{\"pointerBits\":" << sizeof(void*) * 8
        << ",\"exportsVerified\":" << sizeof(exports) / sizeof(exports[0])
        << ",\"localDependenciesVerified\":2,\"lensDefinitions\":" << lenses << "}\n";
    return 0;
}
