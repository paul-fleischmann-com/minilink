//
// tc27x.lsl -- stark vereinfachtes TASKING-LSL fuer minilink (--lsl)
//
// minilink liest daraus nur:
//   * memory{}-Regionen:  type (rom/ram) + Adresse aus map(dest_offset=..)
//   * section_layout/group/select:  Reihenfolge + Segment der Sections
//
// Alles andere (architecture, bus, derivative, core, section_setup, ...)
// wird per Klammer-Skip ueberlesen. Die Adressen hier sind bewusst so
// gewaehlt, dass das erzeugte ELF unter x86-64-Linux lauffaehig ist
// (4K-aligned, gueltiger User-Adressraum, ueberlappungsfrei).
//

architecture DEMO
{
    // von minilink ignoriert -- nur zur Illustration des LSL-Aufbaus
    bus lin { mau = 8; width = 64; }
}

derivative DEMO_SOC
{
    core c0 { architecture = DEMO; }
}

memory rom
{
    type = rom;
    size = 4M;
    map (dest = bus:lin, dest_offset = 0x00400000, size = 4M);
}

memory ram
{
    type = ram;
    size = 4M;
    map (dest = bus:lin, dest_offset = 0x00800000, size = 4M);
}

memory ram2
{
    type = ram;
    size = 4M;
    map (dest = bus:lin, dest_offset = 0x00C00000, size = 4M);
}

section_setup ::linear
{
    // von minilink ignoriert
}

section_layout ::linear
{
    // R-X: Code + Nur-Lese-Daten in die rom-Region
    group (ordered, run_addr = mem:rom)
    {
        select ".text";
        select ".text.*";
        select ".rodata";
        select ".rodata.*";
    }

    // RW: initialisierte Daten + .bss in die ram-Region
    group (ordered, run_addr = mem:ram)
    {
        select ".data";
        select ".data.*";
    }

    group (ordered, run_addr = mem:ram2)
    {
        select ".bss";
        select ".bss.*";
    }
}
