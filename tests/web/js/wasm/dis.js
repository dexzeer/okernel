// debugging aid: a small disassembler for the function bodies the generators produce (prints opcode names with immediates)
(function (G) {
const N = {};
const add = (base, names) => names.forEach((n, i) => { N[base + i] = n; });
add(0x45, ['i32.eqz', 'i32.eq', 'i32.ne', 'i32.lt_s', 'i32.lt_u', 'i32.gt_s', 'i32.gt_u', 'i32.le_s', 'i32.le_u', 'i32.ge_s', 'i32.ge_u', 'i64.eqz', 'i64.eq', 'i64.ne', 'i64.lt_s', 'i64.lt_u', 'i64.gt_s', 'i64.gt_u', 'i64.le_s', 'i64.le_u', 'i64.ge_s', 'i64.ge_u']);
add(0x5b, ['f32.eq', 'f32.ne', 'f32.lt', 'f32.gt', 'f32.le', 'f32.ge', 'f64.eq', 'f64.ne', 'f64.lt', 'f64.gt', 'f64.le', 'f64.ge', 'i32.clz', 'i32.ctz', 'i32.popcnt', 'i32.add', 'i32.sub', 'i32.mul', 'i32.div_s', 'i32.div_u', 'i32.rem_s', 'i32.rem_u', 'i32.and', 'i32.or', 'i32.xor', 'i32.shl', 'i32.shr_s', 'i32.shr_u', 'i32.rotl', 'i32.rotr',
    'i64.clz', 'i64.ctz', 'i64.popcnt', 'i64.add', 'i64.sub', 'i64.mul', 'i64.div_s', 'i64.div_u', 'i64.rem_s', 'i64.rem_u', 'i64.and', 'i64.or', 'i64.xor', 'i64.shl', 'i64.shr_s', 'i64.shr_u', 'i64.rotl', 'i64.rotr',
    'f32.abs', 'f32.neg', 'f32.ceil', 'f32.floor', 'f32.trunc', 'f32.nearest', 'f32.sqrt', 'f32.add', 'f32.sub', 'f32.mul', 'f32.div', 'f32.min', 'f32.max', 'f32.copysign',
    'f64.abs', 'f64.neg', 'f64.ceil', 'f64.floor', 'f64.trunc', 'f64.nearest', 'f64.sqrt', 'f64.add', 'f64.sub', 'f64.mul', 'f64.div', 'f64.min', 'f64.max', 'f64.copysign',
    'i32.wrap_i64', 'i32.trunc_f32_s', 'i32.trunc_f32_u', 'i32.trunc_f64_s', 'i32.trunc_f64_u', 'i64.extend_i32_s', 'i64.extend_i32_u', 'i64.trunc_f32_s', 'i64.trunc_f32_u', 'i64.trunc_f64_s', 'i64.trunc_f64_u',
    'f32.convert_i32_s', 'f32.convert_i32_u', 'f32.convert_i64_s', 'f32.convert_i64_u', 'f32.demote_f64', 'f64.convert_i32_s', 'f64.convert_i32_u', 'f64.convert_i64_s', 'f64.convert_i64_u', 'f64.promote_f32',
    'i32.reinterpret_f32', 'i64.reinterpret_f64', 'f32.reinterpret_i32', 'f64.reinterpret_i64', 'i32.extend8_s', 'i32.extend16_s', 'i64.extend8_s', 'i64.extend16_s', 'i64.extend32_s']);
Object.assign(N, { 0: 'unreachable', 1: 'nop', 2: 'block', 3: 'loop', 4: 'if', 5: 'else', 0xb: 'end', 0xc: 'br', 0xd: 'br_if', 0xe: 'br_table', 0xf: 'return', 0x10: 'call', 0x1a: 'drop', 0x1b: 'select', 0x20: 'local.get', 0x21: 'local.set', 0x22: 'local.tee', 0x23: 'global.get', 0x24: 'global.set' });
add(0x28, ['i32.load', 'i64.load', 'f32.load', 'f64.load', 'i32.load8_s', 'i32.load8_u', 'i32.load16_s', 'i32.load16_u', 'i64.load8_s', 'i64.load8_u', 'i64.load16_s', 'i64.load16_u', 'i64.load32_s', 'i64.load32_u', 'i32.store', 'i64.store', 'f32.store', 'f64.store', 'i32.store8', 'i32.store16', 'i64.store8', 'i64.store16', 'i64.store32']);
G.disasm = function (b) {
    let p = 0, depth = 0;
    const u = () => { let r = 0n, sh = 0n, x; do { x = b[p++]; r |= BigInt(x & 0x7f) << sh; sh += 7n; } while (x & 0x80); return r; };
    const s = (bits) => { let r = 0n, sh = 0n, x; do { x = b[p++]; r |= BigInt(x & 0x7f) << sh; sh += 7n; } while (x & 0x80); if (x & 0x40) r |= -1n << sh; return r; };
    const out = [];
    while (p < b.length) {
        const op = b[p++];
        let t = N[op] || '0x' + op.toString(16), imm = '';
        if (op === 0x41 || op === 0x42) imm = ' ' + s();
        else if (op === 0x43) { imm = ' ' + new Float32Array(new Uint8Array(b.slice(p, p + 4)).buffer)[0]; p += 4; }
        else if (op === 0x44) { imm = ' ' + new Float64Array(new Uint8Array(b.slice(p, p + 8)).buffer)[0]; p += 8; }
        else if (op === 2 || op === 3 || op === 4) { imm = ' bt=0x' + b[p++].toString(16); }
        else if ([0xc, 0xd, 0x10, 0x20, 0x21, 0x22, 0x23, 0x24].includes(op)) imm = ' ' + u();
        else if (op === 0xe) { const n = Number(u()); const l = []; for (let i = 0; i <= n; i++) l.push(u()); imm = ' [' + l.join(',') + ']'; }
        else if (op >= 0x28 && op <= 0x3e) imm = ' a=' + u() + ' off=' + u();
        else if (op === 0xfc) { t = 'fc.' + b[p++]; }
        if (op === 0xb) depth--;
        out.push('  '.repeat(Math.max(depth, 0)) + t + imm);
        if (op === 2 || op === 3 || op === 4) depth++;
    }
    return out.join('\n');
};
})(typeof globalThis !== 'undefined' ? globalThis : this);
