/*
Copyright 2013-present Barefoot Networks, Inc.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include "target.h"

#include "ebpfType.h"

namespace P4::EBPF {

void Target::emitPreamble(Util::SourceCodeBuilder *builder) const { (void)builder; }

void Target::emitTraceMessage(Util::SourceCodeBuilder *builder, const char *format, int argc,
                              ...) const {
    (void)builder;
    (void)format;
    (void)argc;
}

void Target::emitTraceMessage(Util::SourceCodeBuilder *builder, const char *format) const {
    emitTraceMessage(builder, format, 0);
}

//////////////////////////////////////////////////////////////

void KernelSamplesTarget::emitIncludes(Util::SourceCodeBuilder *builder) const {
    builder->append("#include \"ebpf_kernel.h\"\n");
    builder->newline();
}

void KernelSamplesTarget::emitResizeBuffer(Util::SourceCodeBuilder *builder, cstring buffer,
                                           cstring offsetVar) const {
    builder->appendFormat("bpf_skb_adjust_room(%v, %v, 1, 0)", buffer, offsetVar);
}

void KernelSamplesTarget::emitTableLookup(Util::SourceCodeBuilder *builder, cstring tblName,
                                          cstring key, cstring value) const {
    if (!value.isNullOrEmpty()) builder->appendFormat("%s = ", value.c_str());
    builder->appendFormat("BPF_MAP_LOOKUP_ELEM(%v, &%v)", tblName, key);
}

void KernelSamplesTarget::emitTableUpdate(Util::SourceCodeBuilder *builder, cstring tblName,
                                          cstring key, cstring value) const {
    builder->appendFormat("BPF_MAP_UPDATE_ELEM(%v, &%v, &%v, BPF_ANY);", tblName, key, value);
}

void KernelSamplesTarget::emitUserTableUpdate(Util::SourceCodeBuilder *builder, cstring tblName,
                                              cstring key, cstring value) const {
    builder->appendFormat("BPF_USER_MAP_UPDATE_ELEM(%v, &%v, &%v, BPF_ANY);", tblName, key, value);
}

void KernelSamplesTarget::emitTableDecl(Util::SourceCodeBuilder *builder, cstring tblName,
                                        TableKind tableKind, cstring keyType, cstring valueType,
                                        unsigned size) const {
    cstring kind, flags;
    static constexpr std::string_view registerTable = "REGISTER_TABLE(%v, %v, %v, %v, %d)";
    static constexpr std::string_view registerTableWithFlags =
        "REGISTER_TABLE_FLAGS(%v, %v, %v, %v, %d, %v)";

    kind = getBPFMapType(tableKind);

    if (keyType != "u32" && (tableKind == TablePerCPUArray || tableKind == TableArray)) {
        // it's more safe to overwrite user-provided key type,
        // as array map must have u32 key type.
        ::P4::warning(ErrorType::WARN_INVALID,
                      "Invalid key type (%1%) for table kind %2%, replacing with u32", keyType,
                      kind);
        keyType = "u32"_cs;
    } else if (tableKind == TableProgArray && (keyType != "u32" || valueType != "u32")) {
        ::P4::warning(ErrorType::WARN_INVALID,
                      "Invalid key type (%1%) or value type (%2%) for table kind %3%, "
                      "replacing with u32",
                      keyType, valueType, kind);
        keyType = "u32"_cs;
        valueType = "u32"_cs;
    }

    if (tableKind == TableLPMTrie) {
        flags = "BPF_F_NO_PREALLOC"_cs;
    }

    if (flags.isNullOrEmpty()) {
        builder->appendFormat(registerTable, tblName, kind, keyType, valueType, size);
    } else {
        builder->appendFormat(registerTableWithFlags, tblName, kind, keyType, valueType, size,
                              flags);
    }
    builder->newline();
    annotateTableWithBTF(builder, tblName, keyType, valueType);
}

void KernelSamplesTarget::emitTableDeclSpinlock(Util::SourceCodeBuilder *builder, cstring tblName,
                                                TableKind tableKind, cstring keyType,
                                                cstring valueType, unsigned size) const {
    if (tableKind == TableHash || tableKind == TableArray) {
        emitTableDecl(builder, tblName, tableKind, keyType, valueType, size);
    } else {
        BUG("%1%: unsupported table kind with spinlock", tableKind);
    }
}

void KernelSamplesTarget::emitMapInMapDecl(Util::SourceCodeBuilder *builder, cstring innerName,
                                           TableKind innerTableKind, cstring innerKeyType,
                                           cstring innerValueType, unsigned int innerSize,
                                           cstring outerName, TableKind outerTableKind,
                                           cstring outerKeyType, unsigned int outerSize) const {
    if (outerTableKind != TableArray && outerTableKind != TableHash) {
        BUG("Unsupported type of outer map for map-in-map");
    }

    static constexpr std::string_view registerOuterTable =
        "REGISTER_TABLE_OUTER(%v, %v_OF_MAPS, %v, %s, %d, %d, %v)";
    static constexpr std::string_view registerInnerTable =
        "REGISTER_TABLE_INNER(%v, %v, %v, %v, %d, %d, %d)";

    innerMapIndex++;

    cstring kind = getBPFMapType(innerTableKind);
    builder->appendFormat(registerInnerTable, innerName, kind, innerKeyType, innerValueType,
                          innerSize, innerMapIndex, innerMapIndex);
    builder->newline();
    annotateTableWithBTF(builder, innerName, innerKeyType, innerValueType);

    kind = getBPFMapType(outerTableKind);
    cstring keyType = outerTableKind == TableArray ? "__u32"_cs : outerKeyType;
    builder->appendFormat(registerOuterTable, outerName, kind, keyType, "__u32", outerSize,
                          innerMapIndex, innerName);
    builder->newline();
    annotateTableWithBTF(builder, outerName, keyType, "__u32"_cs);
}

void KernelSamplesTarget::emitLicense(Util::SourceCodeBuilder *builder, cstring license) const {
    builder->emitIndent();
    builder->appendFormat(R"(char _license[] SEC("license") = "%v";)", license);
    builder->newline();
}

void KernelSamplesTarget::emitCodeSection(Util::SourceCodeBuilder *builder,
                                          cstring sectionName) const {
    builder->appendFormat("SEC(\"%v\")\n", sectionName);
}

void KernelSamplesTarget::emitMain(Util::SourceCodeBuilder *builder, cstring functionName,
                                   cstring argName) const {
    builder->appendFormat("int %v(SK_BUFF *%v)", functionName, argName);
}

void KernelSamplesTarget::emitPreamble(Util::SourceCodeBuilder *builder) const {
    const char *macro;
    if (emitTraceMessages) {
        macro =
            "#define bpf_trace_message(fmt, ...)                                \\\n"
            "    do {                                                           \\\n"
            "        char ____fmt[] = fmt;                                      \\\n"
            "        bpf_trace_printk(____fmt, sizeof(____fmt), ##__VA_ARGS__); \\\n"
            "    } while(0)";
    } else {
        // With disabled tracing also add this (empty) macro
        // to avoid errors when somewhere it is hardcoded.
        macro = "#define bpf_trace_message(fmt, ...)";
    }
    builder->appendLine(macro);
    builder->newline();
}

// FIXME: Fix terrible implementation
void KernelSamplesTarget::emitTraceMessage(Util::SourceCodeBuilder *builder, const char *format,
                                           int argc, ...) const {
    if (!emitTraceMessages) return;

    cstring msg = cstring(format);
    va_list ap;

    // Older kernels do not append new line when printing message but newer do that,
    // so ensure that printed message ends with '\n'. Empty lines in logs
    // will look better than everything in a single line.
    if (!msg.endsWith("\\n")) msg = msg + "\\n";

    msg = "\""_cs + msg + "\""_cs;
    va_start(ap, argc);
    for (int i = 0; i < argc; ++i) {
        auto arg = va_arg(ap, const char *);
        if (!arg) break;
        msg = msg + ", " + cstring(arg);
    }
    va_end(ap);

    builder->emitIndent();
    builder->appendFormat("bpf_trace_message(%v);", msg);
    builder->newline();
}

void KernelSamplesTarget::annotateTableWithBTF(Util::SourceCodeBuilder *builder, cstring name,
                                               cstring keyType, cstring valueType) const {
    builder->appendFormat("BPF_ANNOTATE_KV_PAIR(%v, %v, %v)", name, keyType, valueType);
    builder->newline();
}

//////////////////////////////////////////////////////////////
void XdpTarget::emitResizeBuffer(Util::SourceCodeBuilder *builder, cstring buffer,
                                 cstring offsetVar) const {
    builder->appendFormat("bpf_xdp_adjust_head(%v, -%v)", buffer, offsetVar);
}

//////////////////////////////////////////////////////////////

void TestTarget::emitIncludes(Util::SourceCodeBuilder *builder) const {
    builder->append("#include \"ebpf_test.h\"\n");
    builder->newline();
}

void TestTarget::emitTableDecl(Util::SourceCodeBuilder *builder, cstring tblName, TableKind,
                               cstring keyType, cstring valueType, unsigned size) const {
    builder->appendFormat("REGISTER_TABLE(%v, 0 /* unused */,", tblName);
    builder->appendFormat("sizeof(%v), sizeof(%v), %d)", keyType, valueType, size);
    builder->newline();
}

//////////////////////////////////////////////////////////////

void BccTarget::emitTableLookup(Util::SourceCodeBuilder *builder, cstring tblName, cstring key,
                                cstring value) const {
    if (!value.isNullOrEmpty()) builder->appendFormat("%s = ", value.c_str());
    builder->appendFormat("%s.lookup(&%s)", tblName.c_str(), key.c_str());
}

void BccTarget::emitTableUpdate(Util::SourceCodeBuilder *builder, cstring tblName, cstring key,
                                cstring value) const {
    builder->appendFormat("%s.update(&%s, &%s);", tblName.c_str(), key.c_str(), value.c_str());
}

void BccTarget::emitUserTableUpdate(Util::SourceCodeBuilder *builder, cstring tblName, cstring key,
                                    cstring value) const {
    builder->appendFormat("bpf_update_elem(%s, &%s, &%s, BPF_ANY);", tblName.c_str(), key.c_str(),
                          value.c_str());
}

void BccTarget::emitIncludes(Util::SourceCodeBuilder *builder) const {
    builder->append(
        "#include <uapi/linux/bpf.h>\n"
        "#include <uapi/linux/if_ether.h>\n"
        "#include <uapi/linux/if_packet.h>\n"
        "#include <uapi/linux/ip.h>\n"
        "#include <linux/skbuff.h>\n"
        "#include <linux/netdevice.h>\n");
}

void BccTarget::emitTableDecl(Util::SourceCodeBuilder *builder, cstring tblName,
                              TableKind tableKind, cstring keyType, cstring valueType,
                              unsigned size) const {
    cstring kind;
    if (tableKind == TableHash)
        kind = "hash"_cs;
    else if (tableKind == TableArray)
        kind = "array"_cs;
    else if (tableKind == TableLPMTrie)
        kind = "lpm_trie"_cs;
    else
        BUG("%1%: unsupported table kind", tableKind);

    builder->appendFormat("BPF_TABLE(\"%s\", %s, %s, %s, %d);", kind.c_str(), keyType.c_str(),
                          valueType.c_str(), tblName.c_str(), size);
    builder->newline();
}

void BccTarget::emitMain(Util::SourceCodeBuilder *builder, cstring functionName,
                         cstring argName) const {
    builder->appendFormat("int %s(struct __sk_buff* %s)", functionName.c_str(), argName.c_str());
}

}  // namespace P4::EBPF
