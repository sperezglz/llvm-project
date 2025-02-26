//===--- ParsedAST.cpp -------------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ParsedAST.h"
#include "../clang-tidy/ClangTidyDiagnosticConsumer.h"
#include "../clang-tidy/ClangTidyModuleRegistry.h"
#include "AST.h"
#include "Compiler.h"
#include "Diagnostics.h"
#include "Headers.h"
#include "IncludeFixer.h"
#include "Logger.h"
#include "SourceCode.h"
#include "Trace.h"
#include "index/CanonicalIncludes.h"
#include "index/Index.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Frontend/Utils.h"
#include "clang/Index/IndexDataConsumer.h"
#include "clang/Index/IndexingAction.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/MacroInfo.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "clang/Sema/Sema.h"
#include "clang/Serialization/ASTWriter.h"
#include "clang/Serialization/PCHContainerOperations.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "clang/Tooling/Syntax/Tokens.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <memory>

// Force the linker to link in Clang-tidy modules.
// clangd doesn't support the static analyzer.
#define CLANG_TIDY_DISABLE_STATIC_ANALYZER_CHECKS
#include "../clang-tidy/ClangTidyForceLinker.h"

namespace clang {
namespace clangd {
namespace {

template <class T> std::size_t getUsedBytes(const std::vector<T> &Vec) {
  return Vec.capacity() * sizeof(T);
}

class DeclTrackingASTConsumer : public ASTConsumer {
public:
  DeclTrackingASTConsumer(std::vector<Decl *> &TopLevelDecls)
      : TopLevelDecls(TopLevelDecls) {}

  bool HandleTopLevelDecl(DeclGroupRef DG) override {
    for (Decl *D : DG) {
      auto &SM = D->getASTContext().getSourceManager();
      if (!isInsideMainFile(D->getLocation(), SM))
        continue;
      if (const NamedDecl *ND = dyn_cast<NamedDecl>(D))
        if (isImplicitTemplateInstantiation(ND))
          continue;

      // ObjCMethodDecl are not actually top-level decls.
      if (isa<ObjCMethodDecl>(D))
        continue;

      TopLevelDecls.push_back(D);
    }
    return true;
  }

private:
  std::vector<Decl *> &TopLevelDecls;
};

class ClangdFrontendAction : public SyntaxOnlyAction {
public:
  std::vector<Decl *> takeTopLevelDecls() { return std::move(TopLevelDecls); }

protected:
  std::unique_ptr<ASTConsumer>
  CreateASTConsumer(CompilerInstance &CI, llvm::StringRef InFile) override {
    return std::make_unique<DeclTrackingASTConsumer>(/*ref*/ TopLevelDecls);
  }

private:
  std::vector<Decl *> TopLevelDecls;
};

// When using a preamble, only preprocessor events outside its bounds are seen.
// This is almost what we want: replaying transitive preprocessing wastes time.
// However this confuses clang-tidy checks: they don't see any #includes!
// So we replay the *non-transitive* #includes that appear in the main-file.
// It would be nice to replay other events (macro definitions, ifdefs etc) but
// this addresses the most common cases fairly cheaply.
class ReplayPreamble : private PPCallbacks {
public:
  // Attach preprocessor hooks such that preamble events will be injected at
  // the appropriate time.
  // Events will be delivered to the *currently registered* PP callbacks.
  static void attach(const IncludeStructure &Includes,
                     CompilerInstance &Clang) {
    auto &PP = Clang.getPreprocessor();
    auto *ExistingCallbacks = PP.getPPCallbacks();
    // No need to replay events if nobody is listening.
    if (!ExistingCallbacks)
      return;
    PP.addPPCallbacks(std::unique_ptr<PPCallbacks>(
        new ReplayPreamble(Includes, ExistingCallbacks,
                           Clang.getSourceManager(), PP, Clang.getLangOpts())));
    // We're relying on the fact that addPPCallbacks keeps the old PPCallbacks
    // around, creating a chaining wrapper. Guard against other implementations.
    assert(PP.getPPCallbacks() != ExistingCallbacks &&
           "Expected chaining implementation");
  }

private:
  ReplayPreamble(const IncludeStructure &Includes, PPCallbacks *Delegate,
                 const SourceManager &SM, Preprocessor &PP,
                 const LangOptions &LangOpts)
      : Includes(Includes), Delegate(Delegate), SM(SM), PP(PP),
        LangOpts(LangOpts) {}

  // In a normal compile, the preamble traverses the following structure:
  //
  // mainfile.cpp
  //   <built-in>
  //     ... macro definitions like __cplusplus ...
  //     <command-line>
  //       ... macro definitions for args like -Dfoo=bar ...
  //   "header1.h"
  //     ... header file contents ...
  //   "header2.h"
  //     ... header file contents ...
  //   ... main file contents ...
  //
  // When using a preamble, the "header1" and "header2" subtrees get skipped.
  // We insert them right after the built-in header, which still appears.
  void FileChanged(SourceLocation Loc, FileChangeReason Reason,
                   SrcMgr::CharacteristicKind Kind, FileID PrevFID) override {
    // It'd be nice if there was a better way to identify built-in headers...
    if (Reason == FileChangeReason::ExitFile &&
        SM.getBuffer(PrevFID)->getBufferIdentifier() == "<built-in>")
      replay();
  }

  void replay() {
    for (const auto &Inc : Includes.MainFileIncludes) {
      const FileEntry *File = nullptr;
      if (Inc.Resolved != "")
        if (auto FE = SM.getFileManager().getFile(Inc.Resolved))
          File = *FE;

      llvm::StringRef WrittenFilename =
          llvm::StringRef(Inc.Written).drop_front().drop_back();
      bool Angled = llvm::StringRef(Inc.Written).startswith("<");

      // Re-lex the #include directive to find its interesting parts.
      llvm::StringRef Src = SM.getBufferData(SM.getMainFileID());
      Lexer RawLexer(SM.getLocForStartOfFile(SM.getMainFileID()), LangOpts,
                     Src.begin(), Src.begin() + Inc.HashOffset, Src.end());
      Token HashTok, IncludeTok, FilenameTok;
      RawLexer.LexFromRawLexer(HashTok);
      assert(HashTok.getKind() == tok::hash);
      RawLexer.setParsingPreprocessorDirective(true);
      RawLexer.LexFromRawLexer(IncludeTok);
      IdentifierInfo *II = PP.getIdentifierInfo(IncludeTok.getRawIdentifier());
      IncludeTok.setIdentifierInfo(II);
      IncludeTok.setKind(II->getTokenID());
      RawLexer.LexIncludeFilename(FilenameTok);

      Delegate->InclusionDirective(
          HashTok.getLocation(), IncludeTok, WrittenFilename, Angled,
          CharSourceRange::getCharRange(FilenameTok.getLocation(),
                                        FilenameTok.getEndLoc()),
          File, "SearchPath", "RelPath", /*Imported=*/nullptr, Inc.FileKind);
      if (File)
        // FIXME: Use correctly named FileEntryRef.
        Delegate->FileSkipped(FileEntryRef(File->getName(), *File), FilenameTok,
                              Inc.FileKind);
      else {
        llvm::SmallString<1> UnusedRecovery;
        Delegate->FileNotFound(WrittenFilename, UnusedRecovery);
      }
    }
  }

  const IncludeStructure &Includes;
  PPCallbacks *Delegate;
  const SourceManager &SM;
  Preprocessor &PP;
  const LangOptions &LangOpts;
};

} // namespace

void dumpAST(ParsedAST &AST, llvm::raw_ostream &OS) {
  AST.getASTContext().getTranslationUnitDecl()->dump(OS, true);
}

llvm::Optional<ParsedAST>
ParsedAST::build(std::unique_ptr<clang::CompilerInvocation> CI,
                 llvm::ArrayRef<Diag> CompilerInvocationDiags,
                 std::shared_ptr<const PreambleData> Preamble,
                 std::unique_ptr<llvm::MemoryBuffer> Buffer,
                 llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> VFS,
                 const SymbolIndex *Index, const ParseOptions &Opts) {
  assert(CI);
  // Command-line parsing sets DisableFree to true by default, but we don't want
  // to leak memory in clangd.
  CI->getFrontendOpts().DisableFree = false;
  const PrecompiledPreamble *PreamblePCH =
      Preamble ? &Preamble->Preamble : nullptr;

  StoreDiags ASTDiags;
  std::string Content = std::string(Buffer->getBuffer());
  std::string Filename =
      std::string(Buffer->getBufferIdentifier()); // Absolute.

  auto Clang = prepareCompilerInstance(std::move(CI), PreamblePCH,
                                       std::move(Buffer), VFS, ASTDiags);
  if (!Clang)
    return None;

  auto Action = std::make_unique<ClangdFrontendAction>();
  const FrontendInputFile &MainInput = Clang->getFrontendOpts().Inputs[0];
  if (!Action->BeginSourceFile(*Clang, MainInput)) {
    log("BeginSourceFile() failed when building AST for {0}",
        MainInput.getFile());
    return None;
  }

  // Set up ClangTidy. Must happen after BeginSourceFile() so ASTContext exists.
  // Clang-tidy has some limitiations to ensure reasonable performance:
  //  - checks don't see all preprocessor events in the preamble
  //  - matchers run only over the main-file top-level decls (and can't see
  //    ancestors outside this scope).
  // In practice almost all checks work well without modifications.
  std::vector<std::unique_ptr<tidy::ClangTidyCheck>> CTChecks;
  ast_matchers::MatchFinder CTFinder;
  llvm::Optional<tidy::ClangTidyContext> CTContext;
  {
    trace::Span Tracer("ClangTidyInit");
    dlog("ClangTidy configuration for file {0}: {1}", Filename,
         tidy::configurationAsText(Opts.ClangTidyOpts));
    tidy::ClangTidyCheckFactories CTFactories;
    for (const auto &E : tidy::ClangTidyModuleRegistry::entries())
      E.instantiate()->addCheckFactories(CTFactories);
    CTContext.emplace(std::make_unique<tidy::DefaultOptionsProvider>(
        tidy::ClangTidyGlobalOptions(), Opts.ClangTidyOpts));
    CTContext->setDiagnosticsEngine(&Clang->getDiagnostics());
    CTContext->setASTContext(&Clang->getASTContext());
    CTContext->setCurrentFile(Filename);
    CTChecks = CTFactories.createChecks(CTContext.getPointer());
    ASTDiags.setLevelAdjuster([&CTContext](DiagnosticsEngine::Level DiagLevel,
                                           const clang::Diagnostic &Info) {
      if (CTContext) {
        std::string CheckName = CTContext->getCheckName(Info.getID());
        bool IsClangTidyDiag = !CheckName.empty();
        if (IsClangTidyDiag) {
          // Check for warning-as-error.
          // We deliberately let this take precedence over suppression comments
          // to match clang-tidy's behaviour.
          if (DiagLevel == DiagnosticsEngine::Warning &&
              CTContext->treatAsError(CheckName)) {
            return DiagnosticsEngine::Error;
          }

          // Check for suppression comment. Skip the check for diagnostics not
          // in the main file, because we don't want that function to query the
          // source buffer for preamble files. For the same reason, we ask
          // shouldSuppressDiagnostic not to follow macro expansions, since
          // those might take us into a preamble file as well.
          bool IsInsideMainFile =
              Info.hasSourceManager() &&
              isInsideMainFile(Info.getLocation(), Info.getSourceManager());
          if (IsInsideMainFile && tidy::shouldSuppressDiagnostic(
                                      DiagLevel, Info, *CTContext,
                                      /* CheckMacroExpansion = */ false)) {
            return DiagnosticsEngine::Ignored;
          }
        }
      }
      return DiagLevel;
    });
    Preprocessor *PP = &Clang->getPreprocessor();
    for (const auto &Check : CTChecks) {
      if (!Check->isLanguageVersionSupported(CTContext->getLangOpts()))
        continue;
      // FIXME: the PP callbacks skip the entire preamble.
      // Checks that want to see #includes in the main file do not see them.
      Check->registerPPCallbacks(Clang->getSourceManager(), PP, PP);
      Check->registerMatchers(&CTFinder);
    }
  }

  // Add IncludeFixer which can recover diagnostics caused by missing includes
  // (e.g. incomplete type) and attach include insertion fixes to diagnostics.
  llvm::Optional<IncludeFixer> FixIncludes;
  auto BuildDir = VFS->getCurrentWorkingDirectory();
  if (Opts.SuggestMissingIncludes && Index && !BuildDir.getError()) {
    auto Style = getFormatStyleForFile(Filename, Content, VFS.get());
    auto Inserter = std::make_shared<IncludeInserter>(
        Filename, Content, Style, BuildDir.get(),
        &Clang->getPreprocessor().getHeaderSearchInfo());
    if (Preamble) {
      for (const auto &Inc : Preamble->Includes.MainFileIncludes)
        Inserter->addExisting(Inc);
    }
    FixIncludes.emplace(Filename, Inserter, *Index,
                        /*IndexRequestLimit=*/5);
    ASTDiags.contributeFixes([&FixIncludes](DiagnosticsEngine::Level DiagLevl,
                                            const clang::Diagnostic &Info) {
      return FixIncludes->fix(DiagLevl, Info);
    });
    Clang->setExternalSemaSource(FixIncludes->unresolvedNameRecorder());
  }

  // Copy over the includes from the preamble, then combine with the
  // non-preamble includes below.
  auto Includes = Preamble ? Preamble->Includes : IncludeStructure{};
  // Replay the preamble includes so that clang-tidy checks can see them.
  if (Preamble)
    ReplayPreamble::attach(Includes, *Clang);
  // Important: collectIncludeStructure is registered *after* ReplayPreamble!
  // Otherwise we would collect the replayed includes again...
  // (We can't *just* use the replayed includes, they don't have Resolved path).
  Clang->getPreprocessor().addPPCallbacks(
      collectIncludeStructureCallback(Clang->getSourceManager(), &Includes));
  // Copy over the macros in the preamble region of the main file, and combine
  // with non-preamble macros below.
  MainFileMacros Macros;
  if (Preamble)
    Macros = Preamble->Macros;
  Clang->getPreprocessor().addPPCallbacks(
      std::make_unique<CollectMainFileMacros>(Clang->getSourceManager(),
                                              Macros));

  // Copy over the includes from the preamble, then combine with the
  // non-preamble includes below.
  CanonicalIncludes CanonIncludes;
  if (Preamble)
    CanonIncludes = Preamble->CanonIncludes;
  else
    CanonIncludes.addSystemHeadersMapping(Clang->getLangOpts());
  std::unique_ptr<CommentHandler> IWYUHandler =
      collectIWYUHeaderMaps(&CanonIncludes);
  Clang->getPreprocessor().addCommentHandler(IWYUHandler.get());

  // Collect tokens of the main file.
  syntax::TokenCollector CollectTokens(Clang->getPreprocessor());

  if (llvm::Error Err = Action->Execute())
    log("Execute() failed when building AST for {0}: {1}", MainInput.getFile(),
        toString(std::move(Err)));

  // We have to consume the tokens before running clang-tidy to avoid collecting
  // tokens from running the preprocessor inside the checks (only
  // modernize-use-trailing-return-type does that today).
  syntax::TokenBuffer Tokens = std::move(CollectTokens).consume();
  std::vector<Decl *> ParsedDecls = Action->takeTopLevelDecls();
  // AST traversals should exclude the preamble, to avoid performance cliffs.
  Clang->getASTContext().setTraversalScope(ParsedDecls);
  {
    // Run the AST-dependent part of the clang-tidy checks.
    // (The preprocessor part ran already, via PPCallbacks).
    trace::Span Tracer("ClangTidyMatch");
    CTFinder.matchAST(Clang->getASTContext());
  }

  // UnitDiagsConsumer is local, we can not store it in CompilerInstance that
  // has a longer lifetime.
  Clang->getDiagnostics().setClient(new IgnoreDiagnostics);
  // CompilerInstance won't run this callback, do it directly.
  ASTDiags.EndSourceFile();
  // XXX: This is messy: clang-tidy checks flush some diagnostics at EOF.
  // However Action->EndSourceFile() would destroy the ASTContext!
  // So just inform the preprocessor of EOF, while keeping everything alive.
  Clang->getPreprocessor().EndSourceFile();

  std::vector<Diag> Diags = CompilerInvocationDiags;
  // Add diagnostics from the preamble, if any.
  if (Preamble)
    Diags.insert(Diags.end(), Preamble->Diags.begin(), Preamble->Diags.end());
  // Finally, add diagnostics coming from the AST.
  {
    std::vector<Diag> D = ASTDiags.take(CTContext.getPointer());
    Diags.insert(Diags.end(), D.begin(), D.end());
  }
  return ParsedAST(std::move(Preamble), std::move(Clang), std::move(Action),
                   std::move(Tokens), std::move(Macros), std::move(ParsedDecls),
                   std::move(Diags), std::move(Includes),
                   std::move(CanonIncludes));
}

ParsedAST::ParsedAST(ParsedAST &&Other) = default;

ParsedAST &ParsedAST::operator=(ParsedAST &&Other) = default;

ParsedAST::~ParsedAST() {
  if (Action) {
    // We already notified the PP of end-of-file earlier, so detach it first.
    // We must keep it alive until after EndSourceFile(), Sema relies on this.
    auto PP = Clang->getPreprocessorPtr(); // Keep PP alive for now.
    Clang->setPreprocessor(nullptr);       // Detach so we don't send EOF again.
    Action->EndSourceFile();               // Destroy ASTContext and Sema.
    // Now Sema is gone, it's safe for PP to go out of scope.
  }
}

ASTContext &ParsedAST::getASTContext() { return Clang->getASTContext(); }

const ASTContext &ParsedAST::getASTContext() const {
  return Clang->getASTContext();
}

Preprocessor &ParsedAST::getPreprocessor() { return Clang->getPreprocessor(); }

std::shared_ptr<Preprocessor> ParsedAST::getPreprocessorPtr() {
  return Clang->getPreprocessorPtr();
}

const Preprocessor &ParsedAST::getPreprocessor() const {
  return Clang->getPreprocessor();
}

llvm::ArrayRef<Decl *> ParsedAST::getLocalTopLevelDecls() {
  return LocalTopLevelDecls;
}

const MainFileMacros &ParsedAST::getMacros() const { return Macros; }

const std::vector<Diag> &ParsedAST::getDiagnostics() const { return Diags; }

std::size_t ParsedAST::getUsedBytes() const {
  auto &AST = getASTContext();
  // FIXME(ibiryukov): we do not account for the dynamically allocated part of
  // Message and Fixes inside each diagnostic.
  std::size_t Total =
      clangd::getUsedBytes(LocalTopLevelDecls) + clangd::getUsedBytes(Diags);

  // FIXME: the rest of the function is almost a direct copy-paste from
  // libclang's clang_getCXTUResourceUsage. We could share the implementation.

  // Sum up variaous allocators inside the ast context and the preprocessor.
  Total += AST.getASTAllocatedMemory();
  Total += AST.getSideTableAllocatedMemory();
  Total += AST.Idents.getAllocator().getTotalMemory();
  Total += AST.Selectors.getTotalMemory();

  Total += AST.getSourceManager().getContentCacheSize();
  Total += AST.getSourceManager().getDataStructureSizes();
  Total += AST.getSourceManager().getMemoryBufferSizes().malloc_bytes;

  if (ExternalASTSource *Ext = AST.getExternalSource())
    Total += Ext->getMemoryBufferSizes().malloc_bytes;

  const Preprocessor &PP = getPreprocessor();
  Total += PP.getTotalMemory();
  if (PreprocessingRecord *PRec = PP.getPreprocessingRecord())
    Total += PRec->getTotalMemory();
  Total += PP.getHeaderSearchInfo().getTotalMemory();

  return Total;
}

const IncludeStructure &ParsedAST::getIncludeStructure() const {
  return Includes;
}

const CanonicalIncludes &ParsedAST::getCanonicalIncludes() const {
  return CanonIncludes;
}

ParsedAST::ParsedAST(std::shared_ptr<const PreambleData> Preamble,
                     std::unique_ptr<CompilerInstance> Clang,
                     std::unique_ptr<FrontendAction> Action,
                     syntax::TokenBuffer Tokens, MainFileMacros Macros,
                     std::vector<Decl *> LocalTopLevelDecls,
                     std::vector<Diag> Diags, IncludeStructure Includes,
                     CanonicalIncludes CanonIncludes)
    : Preamble(std::move(Preamble)), Clang(std::move(Clang)),
      Action(std::move(Action)), Tokens(std::move(Tokens)),
      Macros(std::move(Macros)), Diags(std::move(Diags)),
      LocalTopLevelDecls(std::move(LocalTopLevelDecls)),
      Includes(std::move(Includes)), CanonIncludes(std::move(CanonIncludes)) {
  assert(this->Clang);
  assert(this->Action);
}

llvm::Optional<ParsedAST>
buildAST(PathRef FileName, std::unique_ptr<CompilerInvocation> Invocation,
         llvm::ArrayRef<Diag> CompilerInvocationDiags,
         const ParseInputs &Inputs,
         std::shared_ptr<const PreambleData> Preamble) {
  trace::Span Tracer("BuildAST");
  SPAN_ATTACH(Tracer, "File", FileName);

  auto VFS = Inputs.FS;
  if (Preamble && Preamble->StatCache)
    VFS = Preamble->StatCache->getConsumingFS(std::move(VFS));
  if (VFS->setCurrentWorkingDirectory(Inputs.CompileCommand.Directory)) {
    log("Couldn't set working directory when building the preamble.");
    // We proceed anyway, our lit-tests rely on results for non-existing working
    // dirs.
  }

  return ParsedAST::build(
      std::make_unique<CompilerInvocation>(*Invocation),
      CompilerInvocationDiags, Preamble,
      llvm::MemoryBuffer::getMemBufferCopy(Inputs.Contents, FileName),
      std::move(VFS), Inputs.Index, Inputs.Opts);
}

} // namespace clangd
} // namespace clang
