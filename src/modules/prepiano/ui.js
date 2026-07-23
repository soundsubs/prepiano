/*
 * PrePiano - minimal Shadow UI screen.
 *
 * Shadow UI already auto-renders the chain_params / ui_hierarchy from
 * module.json, so this file is intentionally light: it just draws a title
 * card and a live readout of the eight knobs, and forwards pad/keyboard MIDI
 * straight to the DSP. Parameter editing itself is handled by the host menu.
 *
 * Uses only documented primitives (constants.mjs, clear_screen/print, tick,
 * host_module_get_param). Safe to delete if you prefer the stock UI.
 */
import {
    MidiNoteOn, MidiNoteOff
} from '../../shared/constants.mjs';

const KNOBS = [
    ["Felt", "felt"], ["Atk", "attack"], ["Dcy", "decay"], ["Gge", "gauge"],
    ["Hmr", "hammer"], ["Sym", "symp"], ["Dst", "disturb"], ["Rev", "reverb"]
];

function bar(v) {
    // 0..1 -> a 5-char mini meter
    const n = Math.max(0, Math.min(5, Math.round(v * 5)));
    return "█".repeat(n) + "·".repeat(5 - n);
}

globalThis.init = function () {
    clear_screen();
    print(2, 2, "PrePiano", 2);
    host_flush_display();
};

globalThis.tick = function () {
    clear_screen();
    print(2, 1, "PrePiano", 1);
    // two rows of four knobs
    for (let i = 0; i < 8; i++) {
        const [label, key] = KNOBS[i];
        let v = 0.0;
        try { v = parseFloat(host_module_get_param(key)) || 0.0; } catch (e) {}
        const col = (i % 4) * 32;
        const row = 16 + Math.floor(i / 4) * 20;
        print(col + 2, row, label, 1);
        print(col + 2, row + 9, bar(v), 1);
    }
    host_flush_display();
};

// Forward Move's pads / external keyboard straight into the DSP.
globalThis.onMidiMessageInternal = function (data) {
    const status = data[0] & 0xf0;
    if (status === 0x90 || status === 0x80 || status === 0xb0) {
        host_module_send_midi([data[0], data[1], data[2]], "internal");
    }
};

globalThis.onMidiMessageExternal = function (data) {
    host_module_send_midi([data[0], data[1], data[2]], "external");
};
