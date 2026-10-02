#!/usr/bin/env python3
"""glgen.py: the OpenGL ES forwarding code from gpu/gles32.api.

  guest/gl_gen.h   the guest driver's GL entry points (libGLES_aoi.so, guest/gles.c):
                   each packs its arguments into 64-bit slots and makes the private
                   syscall AOI_SYS_GL (core/gpu.h) with its function number
  gpu/gl_gen.h     the host side (gpu/host.c): a switch that unpacks the slots and
                   calls the host's GLES function, pointers made host pointers

A pointer argument is guest memory. The host needs its length to use it (guest memory
is contiguous only within 2 MiB chunks, core/vm.h), so each one has a rule below: a C
expression for its length in bytes (arguments by name), "STR" for a NUL-terminated
string, or, for a pointer that is a buffer offset while a buffer is bound, the binding
to look at. Without a rule a pointer gets a 4 KiB window and a warning when used.
Functions in SPECIAL are written by hand on the host (gpu/host.c: client vertex arrays,
mapped buffers, sync objects, strings returned, string arrays).

usage: tools/glgen.py   (rewrites both headers)
"""
import os
import re
import sys

DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SPECIAL = {
    "glGetString", "glGetStringi", "glMapBufferRange", "glUnmapBuffer", "glFlushMappedBufferRange",
    "glGetBufferPointerv", "glGetPointerv", "glGetVertexAttribPointerv",
    "glShaderSource", "glTransformFeedbackVaryings", "glGetUniformIndices", "glCreateShaderProgramv",
    "glVertexAttribPointer", "glVertexAttribIPointer", "glEnableVertexAttribArray",
    "glDisableVertexAttribArray", "glBindVertexArray",
    "glDrawArrays", "glDrawElements", "glDrawRangeElements", "glDrawArraysInstanced",
    "glDrawElementsInstanced", "glDrawElementsBaseVertex", "glDrawRangeElementsBaseVertex",
    "glDrawElementsInstancedBaseVertex",
    "glFenceSync", "glDeleteSync", "glIsSync", "glClientWaitSync", "glWaitSync", "glGetSynciv",
    "glDebugMessageCallback",
}

N4 = "4 * (GLsizeiptr)n"
UNIFORM = {"1": 1, "2": 2, "3": 3, "4": 4}
MAT = {"2": 4, "3": 9, "4": 16, "2x3": 6, "3x2": 6, "2x4": 8, "4x2": 8, "3x4": 12, "4x3": 12}
GET64 = "64"
WIN = "512"

RULES = {
    "glBindAttribLocation": {"name": "STR"},
    "glBufferData": {"data": "size"},
    "glBufferSubData": {"data": "size"},
    "glCompressedTexImage2D": {"data": ("imageSize", "GL_PIXEL_UNPACK_BUFFER_BINDING")},
    "glCompressedTexImage3D": {"data": ("imageSize", "GL_PIXEL_UNPACK_BUFFER_BINDING")},
    "glCompressedTexSubImage2D": {"data": ("imageSize", "GL_PIXEL_UNPACK_BUFFER_BINDING")},
    "glCompressedTexSubImage3D": {"data": ("imageSize", "GL_PIXEL_UNPACK_BUFFER_BINDING")},
    "glTexImage2D": {"pixels": ("img_unpack(width, height, 1, format, type)", "GL_PIXEL_UNPACK_BUFFER_BINDING")},
    "glTexImage3D": {"pixels": ("img_unpack(width, height, depth, format, type)", "GL_PIXEL_UNPACK_BUFFER_BINDING")},
    "glTexSubImage2D": {"pixels": ("img_unpack(width, height, 1, format, type)", "GL_PIXEL_UNPACK_BUFFER_BINDING")},
    "glTexSubImage3D": {"pixels": ("img_unpack(width, height, depth, format, type)", "GL_PIXEL_UNPACK_BUFFER_BINDING")},
    "glReadPixels": {"pixels": ("img_pack(width, height, format, type)", "GL_PIXEL_PACK_BUFFER_BINDING")},
    "glReadnPixels": {"data": ("bufSize", "GL_PIXEL_PACK_BUFFER_BINDING")},
    "glDrawBuffers": {"bufs": N4},
    "glInvalidateFramebuffer": {"attachments": "4 * (GLsizeiptr)numAttachments"},
    "glInvalidateSubFramebuffer": {"attachments": "4 * (GLsizeiptr)numAttachments"},
    "glGetActiveAttrib": {"length": "4", "size": "4", "type": "4", "name": "bufSize"},
    "glGetActiveUniform": {"length": "4", "size": "4", "type": "4", "name": "bufSize"},
    "glGetTransformFeedbackVarying": {"length": "4", "size": "4", "type": "4", "name": "bufSize"},
    "glGetAttachedShaders": {"count": "4", "shaders": "4 * (GLsizeiptr)maxCount"},
    "glGetAttribLocation": {"name": "STR"},
    "glGetUniformLocation": {"name": "STR"},
    "glGetFragDataLocation": {"name": "STR"},
    "glGetUniformBlockIndex": {"uniformBlockName": "STR"},
    "glGetProgramResourceIndex": {"name": "STR"},
    "glGetProgramResourceLocation": {"name": "STR"},
    "glGetProgramInfoLog": {"length": "4", "infoLog": "bufSize"},
    "glGetShaderInfoLog": {"length": "4", "infoLog": "bufSize"},
    "glGetProgramPipelineInfoLog": {"length": "4", "infoLog": "bufSize"},
    "glGetShaderSource": {"length": "4", "source": "bufSize"},
    "glGetShaderPrecisionFormat": {"range": "8", "precision": "4"},
    "glGetActiveUniformBlockName": {"length": "4", "uniformBlockName": "bufSize"},
    "glGetActiveUniformBlockiv": {"params": "4096"},
    "glGetActiveUniformsiv": {"uniformIndices": "4 * (GLsizeiptr)uniformCount", "params": "4 * (GLsizeiptr)uniformCount"},
    "glGetProgramBinary": {"length": "4", "binaryFormat": "4", "binary": "bufSize"},
    "glProgramBinary": {"binary": "length"},
    "glGetProgramResourceName": {"length": "4", "name": "bufSize"},
    "glGetProgramResourceiv": {"props": "4 * (GLsizeiptr)propCount", "length": "4", "params": "4 * (GLsizeiptr)bufSize"},
    "glGetObjectLabel": {"length": "4", "label": "bufSize"},
    "glGetObjectPtrLabel": {"ptr": "0", "length": "4", "label": "bufSize"},
    "glObjectLabel": {"label": "strn(length)"},
    "glObjectPtrLabel": {"ptr": "0", "label": "strn(length)"},
    "glPushDebugGroup": {"message": "strn(length)"},
    "glDebugMessageInsert": {"buf": "strn(length)"},
    "glDebugMessageControl": {"ids": "4 * (GLsizeiptr)count"},
    "glGetDebugMessageLog": {"sources": "4 * (GLsizeiptr)count", "types": "4 * (GLsizeiptr)count",
                             "ids": "4 * (GLsizeiptr)count", "severities": "4 * (GLsizeiptr)count",
                             "lengths": "4 * (GLsizeiptr)count", "messageLog": "bufSize"},
    "glGetInternalformativ": {"params": "4 * (GLsizeiptr)bufSize"},
    "glGetMultisamplefv": {"val": "8"},
    "glShaderBinary": {"shaders": "4 * (GLsizeiptr)count", "binary": "length"},
    "glGetnUniformfv": {"params": "bufSize"},
    "glGetnUniformiv": {"params": "bufSize"},
    "glGetnUniformuiv": {"params": "bufSize"},
    "glClearBufferiv": {"value": "16"},
    "glClearBufferuiv": {"value": "16"},
    "glClearBufferfv": {"value": "16"},
    "glVertexAttrib1fv": {"v": "4"},
    "glVertexAttrib2fv": {"v": "8"},
    "glVertexAttrib3fv": {"v": "12"},
    "glVertexAttrib4fv": {"v": "16"},
    "glVertexAttribI4iv": {"v": "16"},
    "glVertexAttribI4uiv": {"v": "16"},
    "glDrawArraysIndirect": {"indirect": ("0", "GL_DRAW_INDIRECT_BUFFER_BINDING")},
    "glDrawElementsIndirect": {"indirect": ("0", "GL_DRAW_INDIRECT_BUFFER_BINDING")},
}


def parse():
    fns = []
    for line in open(os.path.join(DIR, "gpu", "gles32.api")):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        m = re.match(r"(.+?)\s*(gl\w+)\((.*)\)$", line)
        ret, name, params = m.group(1).strip(), m.group(2), m.group(3).strip()
        args = []
        if params and params != "void":
            for p in params.split(","):
                p = p.strip()
                pm = re.match(r"(.*?)(\w+)$", p)
                args.append((pm.group(1).strip(), pm.group(2)))
        fns.append((ret, name, args))
    return fns


def is_ptr(t):
    return "*" in t


def is_float(t):
    return t in ("GLfloat", "GLclampf")


def rule(name, an, args):
    r = RULES.get(name, {}).get(an)
    if r is not None:
        return r
    if re.match(r"glGen\w+s$", name) or re.match(r"glDelete\w+s$", name) or name == "glCreateProgramPipelines":
        n = [a for t, a in args if t == "GLsizei"][0]
        return "4 * (GLsizeiptr)%s" % n
    m = re.match(r"gl(?:Program)?Uniform([1-4])(f|i|ui)v$", name)
    if m:
        return "%d * 4 * (GLsizeiptr)count" % UNIFORM[m.group(1)]
    m = re.match(r"gl(?:Program)?UniformMatrix(\dx\d|\d)fv$", name)
    if m:
        return "%d * 4 * (GLsizeiptr)count" % MAT[m.group(1)]
    if re.match(r"glGet\w+i_v$", name) or name in ("glGetBooleanv", "glGetFloatv", "glGetIntegerv", "glGetInteger64v"):
        return WIN
    if re.match(r"glGet\w+v$", name) or re.match(r"gl(Tex|Sampler)Parameter\w*v$", name):
        return GET64
    return None


def guest(fns):
    out = ["/* Generated by tools/glgen.py from gpu/gles32.api: do not edit. */", ""]
    for i, (ret, name, args) in enumerate(fns):
        out.append("#define AOI_GL_%s %d" % (name, i))
    out.append("#define AOI_GL_COUNT %d" % len(fns))
    out.append("")
    for i, (ret, name, args) in enumerate(fns):
        params = ", ".join("%s %s" % (t, a) if not t.endswith("*") else "%s%s" % (t, a) for t, a in args) or "void"
        body = ["    uint64_t aoi_s[%d];" % max(1, len(args))]
        for k, (t, a) in enumerate(args):
            if is_ptr(t) or t in ("GLsync", "GLDEBUGPROC"):
                body.append("    aoi_s[%d] = (uint64_t)(uintptr_t)%s;" % (k, a))
            elif is_float(t):
                body.append("    aoi_s[%d] = fbits(%s);" % (k, a))
            elif t in ("GLint", "GLsizei", "GLintptr", "GLsizeiptr", "GLint64", "GLshort", "GLbyte", "GLfixed"):
                body.append("    aoi_s[%d] = (uint64_t)(int64_t)%s;" % (k, a))
            else:
                body.append("    aoi_s[%d] = (uint64_t)%s;" % (k, a))
        call = "aoi_gl(AOI_GL_%s, aoi_s)" % name
        if ret == "void":
            body.append("    %s;" % call)
        elif is_ptr(ret) or ret == "GLsync":
            body.append("    return (%s)(uintptr_t)%s;" % (ret, call))
        else:
            body.append("    return (%s)%s;" % (ret, call))
        out.append("EXPORT %s %s(%s)\n{\n%s\n}\n" % (ret, name, params, "\n".join(body)))
    out.append("static const struct { const char *name; void *fn; } aoi_gl_procs[] = {")
    for ret, name, args in fns:
        out.append('    { "%s", (void *)%s },' % (name, name))
    out.append("};")
    return "\n".join(out) + "\n"


def host(fns):
    out = ["/* Generated by tools/glgen.py from gpu/gles32.api: do not edit. */", ""]
    out.append("static const char *const gl_names[] = {")
    out += ['    "%s",' % name for ret, name, args in fns]
    out.append("};")
    out.append("")
    out.append("static uint64_t gl_dispatch(struct gl_call *c, unsigned id, const uint64_t *slot)")
    out.append("{")
    out.append("    switch (id) {")
    for i, (ret, name, args) in enumerate(fns):
        if name in SPECIAL:
            out.append("    case %d: return special_%s(c, slot);" % (i, name))
            continue
        lines = []
        for k, (t, a) in enumerate(args):
            if is_ptr(t):
                continue
            if is_float(t):
                lines.append("        %s %s = F(slot[%d]);" % (t, a, k))
            else:
                lines.append("        %s %s = (%s)slot[%d];" % (t, a, t, k))
        for k, (t, a) in enumerate(args):
            if not is_ptr(t):
                continue
            r = rule(name, a, args)
            base = t.replace("const", "").strip()
            out_dir = "const" not in t
            binding = None
            if isinstance(r, tuple):
                r, binding = r
            if r is None:
                expr = 'gl_ptr(c, slot[%d], 4096, %d, "%s %s")' % (k, int(out_dir), name, a)
            elif r == "STR":
                expr = "gl_str(c, slot[%d], -1)" % k
            elif r.startswith("strn("):
                expr = "gl_str(c, slot[%d], %s)" % (k, r[5:-1])
            else:
                expr = "gl_ptr(c, slot[%d], (GLsizeiptr)(%s), %d, NULL)" % (k, r, int(out_dir))
            if binding:
                expr = "gl_bound(%s) ? (void *)(uintptr_t)slot[%d] : %s" % (binding, k, expr)
            lines.append("        %s %s = (%s)(%s);" % (t, a, t, expr))
        call = "%s(%s)" % (name, ", ".join(a for t, a in args))
        if ret == "void":
            lines.append("        %s;" % call)
            lines.append("        return 0;")
        else:
            lines.append("        return (uint64_t)%s;" % call)
        out.append("    case %d: {    /* %s */" % (i, name))
        out += lines
        out.append("    }")
    out.append("    }")
    out.append("    return 0;")
    out.append("}")
    return "\n".join(out) + "\n"


def main():
    fns = parse()
    names = {n for r, n, a in fns}
    missing = SPECIAL - names
    if missing:
        sys.exit("SPECIAL not in the API: %s" % sorted(missing))
    with open(os.path.join(DIR, "guest", "gl_gen.h"), "w") as f:
        f.write(guest(fns))
    with open(os.path.join(DIR, "gpu", "gl_gen.h"), "w") as f:
        f.write(host(fns))
    print("%d functions, %d by hand" % (len(fns), len(SPECIAL)))


if __name__ == "__main__":
    main()
