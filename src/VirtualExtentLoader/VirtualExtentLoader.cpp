// VirtualExtentLoader.cpp : This file contains the 'main' function. Program execution begins and ends there.
//


#include <Windows.h>
#include <iostream>

typedef int (*VE_Start_Fn)();

int main() {

    HMODULE open_xr = LoadLibraryA("C:\\Users\\calle\\projects\\VirtualExtent\\src\\VirualExtentDll\\x64\\Debug\\openxr_loader.dll");
    if (!open_xr) {
        std::cerr << "Failed to load openxr_loader.dll\n";
        DWORD err = GetLastError();
        std::cerr << "Error code: " << err << "\n";
        return 1;
    }


    HMODULE h = LoadLibraryA("C:\\Users\\calle\\projects\\VirtualExtent\\src\\VirualExtentDll\\x64\\Debug\\VirualExtentDll.dll");
    if (!h) {
        std::cerr << "Failed to load VirtualExtent dll\n";
        DWORD err = GetLastError();
        std::cerr << "Error code: " << err << "\n";
        return 1;
    }

    auto VE_Start = (VE_Start_Fn)GetProcAddress(h, "VE_Start");

    if (!VE_Start) {
        std::cerr << "Failed to get exported functions\n";
        return 1;
    }

    VE_Start();  // Run the OpenXR loop

    FreeLibrary(h);
    return 0;
}

// Run program: Ctrl + F5 or Debug > Start Without Debugging menu
// Debug program: F5 or Debug > Start Debugging menu

// Tips for Getting Started: 
//   1. Use the Solution Explorer window to add/manage files
//   2. Use the Team Explorer window to connect to source control
//   3. Use the Output window to see build output and other messages
//   4. Use the Error List window to view errors
//   5. Go to Project > Add New Item to create new code files, or Project > Add Existing Item to add existing code files to the project
//   6. In the future, to open this project again, go to File > Open > Project and select the .sln file
