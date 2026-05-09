#pragma once
#include <cstdint>

// Fake Microsoft.UI.h for CMake builds without midl generation
namespace ABI {
    namespace Microsoft {
        namespace UI {
            struct WindowId { uint64_t Value; };
            struct DisplayId { uint64_t Value; };
            struct IconId { uint64_t Value; };
        }
    }
}

// Map the non-ABI namespace for C++ code that might use it
namespace Microsoft {
    namespace UI {
        using WindowId = ABI::Microsoft::UI::WindowId;
        using DisplayId = ABI::Microsoft::UI::DisplayId;
        using IconId = ABI::Microsoft::UI::IconId;
    }
}
