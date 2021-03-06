/* Copyright 2017-2018 ccls Authors

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "clang_tu.hh"

#include "config.hh"
#include "platform.hh"

#include <clang/AST/Type.h>
#include <clang/Lex/Lexer.h>
#include <llvm/Support/Path.h>

using namespace clang;

namespace ccls {
std::string pathFromFileEntry(const FileEntry &file) {
  std::string ret = normalizePath(file.getName());
  // Resolve symlinks outside of workspace folders, e.g. /usr/include/c++/7.3.0
  return normalizeFolder(ret) ? ret : realPath(ret);
}

static Pos decomposed2LineAndCol(const SourceManager &sm,
                                 std::pair<FileID, unsigned> i) {
  int l = sm.getLineNumber(i.first, i.second) - 1,
      c = sm.getColumnNumber(i.first, i.second) - 1;
  bool invalid = false;
  StringRef buf = sm.getBufferData(i.first, &invalid);
  if (!invalid) {
    StringRef p = buf.substr(i.second - c, c);
    c = 0;
    for (size_t i = 0; i < p.size();)
      if (c++, (uint8_t)p[i++] >= 128)
        while (i < p.size() && (uint8_t)p[i] >= 128 && (uint8_t)p[i] < 192)
          i++;
  }
  return {(uint16_t)std::min<int>(l, UINT16_MAX),
          (int16_t)std::min<int>(c, INT16_MAX)};
}

Range fromCharSourceRange(const SourceManager &sm, const LangOptions &lang,
                          CharSourceRange csr, FileID *fid) {
  SourceLocation bloc = csr.getBegin(), eloc = csr.getEnd();
  std::pair<FileID, unsigned> binfo = sm.getDecomposedLoc(bloc),
                              einfo = sm.getDecomposedLoc(eloc);
  if (csr.isTokenRange())
    einfo.second += Lexer::MeasureTokenLength(eloc, sm, lang);
  if (fid)
    *fid = binfo.first;
  return {decomposed2LineAndCol(sm, binfo), decomposed2LineAndCol(sm, einfo)};
}

Range fromTokenRange(const SourceManager &sm, const LangOptions &lang,
                     SourceRange sr, FileID *fid) {
  return fromCharSourceRange(sm, lang, CharSourceRange::getTokenRange(sr), fid);
}

Range fromTokenRangeDefaulted(const SourceManager &sm, const LangOptions &lang,
                              SourceRange sr, FileID fid, Range range) {
  auto decomposed = sm.getDecomposedLoc(sm.getExpansionLoc(sr.getBegin()));
  if (decomposed.first == fid)
    range.start = decomposed2LineAndCol(sm, decomposed);
  SourceLocation loc = sm.getExpansionLoc(sr.getEnd());
  decomposed = sm.getDecomposedLoc(loc);
  if (decomposed.first == fid) {
    decomposed.second += Lexer::MeasureTokenLength(loc, sm, lang);
    range.end = decomposed2LineAndCol(sm, decomposed);
  }
  return range;
}

std::unique_ptr<CompilerInvocation>
buildCompilerInvocation(const std::string &main, std::vector<const char *> args,
                        IntrusiveRefCntPtr<llvm::vfs::FileSystem> vfs) {
  std::string save = "-resource-dir=" + g_config->clang.resourceDir;
  args.push_back(save.c_str());
  IntrusiveRefCntPtr<DiagnosticsEngine> diags(
      CompilerInstance::createDiagnostics(new DiagnosticOptions,
                                          new IgnoringDiagConsumer, true));
  std::unique_ptr<CompilerInvocation> ci =
      createInvocationFromCommandLine(args, diags, vfs);
  if (ci) {
    ci->getDiagnosticOpts().IgnoreWarnings = true;
    ci->getFrontendOpts().DisableFree = false;
    ci->getLangOpts()->SpellChecking = false;
    auto &isec = ci->getFrontendOpts().Inputs;
    if (isec.size())
      isec[0] = FrontendInputFile(main, isec[0].getKind(), isec[0].isSystem());
  }
  return ci;
}

// clang::BuiltinType::getName without PrintingPolicy
const char *clangBuiltinTypeName(int kind) {
  switch (BuiltinType::Kind(kind)) {
  case BuiltinType::Void:
    return "void";
  case BuiltinType::Bool:
    return "bool";
  case BuiltinType::Char_S:
    return "char";
  case BuiltinType::Char_U:
    return "char";
  case BuiltinType::SChar:
    return "signed char";
  case BuiltinType::Short:
    return "short";
  case BuiltinType::Int:
    return "int";
  case BuiltinType::Long:
    return "long";
  case BuiltinType::LongLong:
    return "long long";
  case BuiltinType::Int128:
    return "__int128";
  case BuiltinType::UChar:
    return "unsigned char";
  case BuiltinType::UShort:
    return "unsigned short";
  case BuiltinType::UInt:
    return "unsigned int";
  case BuiltinType::ULong:
    return "unsigned long";
  case BuiltinType::ULongLong:
    return "unsigned long long";
  case BuiltinType::UInt128:
    return "unsigned __int128";
  case BuiltinType::Half:
    return "__fp16";
  case BuiltinType::Float:
    return "float";
  case BuiltinType::Double:
    return "double";
  case BuiltinType::LongDouble:
    return "long double";
  case BuiltinType::ShortAccum:
    return "short _Accum";
  case BuiltinType::Accum:
    return "_Accum";
  case BuiltinType::LongAccum:
    return "long _Accum";
  case BuiltinType::UShortAccum:
    return "unsigned short _Accum";
  case BuiltinType::UAccum:
    return "unsigned _Accum";
  case BuiltinType::ULongAccum:
    return "unsigned long _Accum";
  case BuiltinType::BuiltinType::ShortFract:
    return "short _Fract";
  case BuiltinType::BuiltinType::Fract:
    return "_Fract";
  case BuiltinType::BuiltinType::LongFract:
    return "long _Fract";
  case BuiltinType::BuiltinType::UShortFract:
    return "unsigned short _Fract";
  case BuiltinType::BuiltinType::UFract:
    return "unsigned _Fract";
  case BuiltinType::BuiltinType::ULongFract:
    return "unsigned long _Fract";
  case BuiltinType::BuiltinType::SatShortAccum:
    return "_Sat short _Accum";
  case BuiltinType::BuiltinType::SatAccum:
    return "_Sat _Accum";
  case BuiltinType::BuiltinType::SatLongAccum:
    return "_Sat long _Accum";
  case BuiltinType::BuiltinType::SatUShortAccum:
    return "_Sat unsigned short _Accum";
  case BuiltinType::BuiltinType::SatUAccum:
    return "_Sat unsigned _Accum";
  case BuiltinType::BuiltinType::SatULongAccum:
    return "_Sat unsigned long _Accum";
  case BuiltinType::BuiltinType::SatShortFract:
    return "_Sat short _Fract";
  case BuiltinType::BuiltinType::SatFract:
    return "_Sat _Fract";
  case BuiltinType::BuiltinType::SatLongFract:
    return "_Sat long _Fract";
  case BuiltinType::BuiltinType::SatUShortFract:
    return "_Sat unsigned short _Fract";
  case BuiltinType::BuiltinType::SatUFract:
    return "_Sat unsigned _Fract";
  case BuiltinType::BuiltinType::SatULongFract:
    return "_Sat unsigned long _Fract";
  case BuiltinType::Float16:
    return "_Float16";
  case BuiltinType::Float128:
    return "__float128";
  case BuiltinType::WChar_S:
  case BuiltinType::WChar_U:
    return "wchar_t";
  case BuiltinType::Char8:
    return "char8_t";
  case BuiltinType::Char16:
    return "char16_t";
  case BuiltinType::Char32:
    return "char32_t";
  case BuiltinType::NullPtr:
    return "nullptr_t";
  case BuiltinType::Overload:
    return "<overloaded function type>";
  case BuiltinType::BoundMember:
    return "<bound member function type>";
  case BuiltinType::PseudoObject:
    return "<pseudo-object type>";
  case BuiltinType::Dependent:
    return "<dependent type>";
  case BuiltinType::UnknownAny:
    return "<unknown type>";
  case BuiltinType::ARCUnbridgedCast:
    return "<ARC unbridged cast type>";
  case BuiltinType::BuiltinFn:
    return "<builtin fn type>";
  case BuiltinType::ObjCId:
    return "id";
  case BuiltinType::ObjCClass:
    return "Class";
  case BuiltinType::ObjCSel:
    return "SEL";
  case BuiltinType::OCLSampler:
    return "sampler_t";
  case BuiltinType::OCLEvent:
    return "event_t";
  case BuiltinType::OCLClkEvent:
    return "clk_event_t";
  case BuiltinType::OCLQueue:
    return "queue_t";
  case BuiltinType::OCLReserveID:
    return "reserve_id_t";
  case BuiltinType::OMPArraySection:
    return "<OpenMP array section type>";
  default:
    return "";
  }
}
} // namespace ccls
