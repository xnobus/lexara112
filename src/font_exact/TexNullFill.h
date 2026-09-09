#pragma once

// Latka na crash klienta 1.12 przy tekstury, ktorej nie da sie wczytac.
// Szczegoly i disasm: docs/tex-null-fill.md.
namespace TexNullFill {
    // Wola sie z Entry.cpp, WEWNATRZ otwartej transakcji Detours i PO
    // MSDF::initialize() (to ono wczytuje lexara112.cfg). Latka jest
    // niezalezna od msdf_enabled - dotyczy tekstur, nie czcionek.
    void initialize();
}
