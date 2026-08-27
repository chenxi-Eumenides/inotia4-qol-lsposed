'use strict';

// 临时修复 save0 的角色加载链。仅用于一次性进入存档并调用游戏原生保存；
// 保存完成后必须重启进程，不能把这些指令 patch 作为长期运行逻辑。
// 适用当前 libgame.so：所有偏移均相对模块加载基址，换版本必须重新核对反汇编。
const TARGET_SLOT = 0;
const PATCHES = [
    [0x129a94, 0xd2800016, 0xf8008700, 'SAVE_LoadSaveSlot: attach main hero'],
    [0x129a98, 0xf0000e5a, 0x52800076, 'SAVE_LoadSaveSlot: skip party indexes'],
    [0x129a9c, 0xf9409340, 0x14000011, 'SAVE_LoadSaveSlot: skip empty-party loop'],
    [0x129d04, 0x38e06b9b, 0x5280001b, 'SAVE_LoadCharacterAll: force index 0'],
    [0x129d1c, 0x9100079c, 0x5280007c, 'SAVE_LoadCharacterAll: stop after first'],
];

const module = Process.findModuleByName('libgame.so');
if (module === null) throw new Error('libgame.so is not loaded');

// 先完整校验，避免前几条已写入后才发现版本不匹配。
for (const [offset, original, , label] of PATCHES) {
    const current = module.base.add(offset).readU32();
    if (current !== original) {
        throw new Error(`[${label}] opcode mismatch at 0x${offset.toString(16)}: ` +
            `got 0x${current.toString(16)}, want 0x${original.toString(16)}`);
    }
}

const applied = [];
try {
    for (const [offset, original, replacement, label] of PATCHES) {
        const address = module.base.add(offset);
        Memory.patchCode(address, 4, code => code.writeU32(replacement));
        applied.push([offset, replacement, original, label]);
        console.log(`[${label}] 0x${offset.toString(16)}: ` +
            `0x${original.toString(16)} -> 0x${replacement.toString(16)}`);
    }
} catch (error) {
    for (const [offset, replacement, original] of applied.reverse()) {
        const address = module.base.add(offset);
        Memory.patchCode(address, 4, code => code.writeU32(original));
        console.log(`[rollback] 0x${offset.toString(16)}: ` +
            `0x${replacement.toString(16)} -> 0x${original.toString(16)}`);
    }
    throw error;
}

console.log(`save${TARGET_SLOT} repair patch installed; enter that slot, save in-game, then restart without this script`);

function forceTargetPlayerIndex() {
    const indicesPtr = module.base.add(0x2f4000 + 0x120).readPointer();
    if (indicesPtr.isNull()) throw new Error('player index array is null');
    indicesPtr.add(0).writeS8(0);
    console.log(`[save${TARGET_SLOT}] player_indices[0] = 0`);
}

// SAVE_LoadSaveSlot reads the Player block and can restore -1 after the
// instruction patch. Restrict the write to save0 and run it after the full
// three-slot refresh as well as after the target-slot preflight load.
for (const [offset, targetSlot] of [[0x129b38, null], [0x1298dc, TARGET_SLOT]]) {
    Interceptor.attach(module.base.add(offset), {
        onEnter(args) {
            this.slot = targetSlot === null ? null : args[0].toInt32();
        },
        onLeave() {
            if (targetSlot === null || this.slot === TARGET_SLOT) forceTargetPlayerIndex();
        },
    });
}
