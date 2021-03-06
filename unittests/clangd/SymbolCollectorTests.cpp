//===-- SymbolCollectorTests.cpp  -------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "index/SymbolCollector.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/FileSystemOptions.h"
#include "clang/Basic/VirtualFileSystem.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Index/IndexingAction.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/MemoryBuffer.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include <memory>
#include <string>

using testing::Eq;
using testing::Field;
using testing::UnorderedElementsAre;

// GMock helpers for matching Symbol.
MATCHER_P(QName, Name, "") { return arg.second.QualifiedName == Name; }

namespace clang {
namespace clangd {

namespace {
class SymbolIndexActionFactory : public tooling::FrontendActionFactory {
public:
  SymbolIndexActionFactory() = default;

  clang::FrontendAction *create() override {
    index::IndexingOptions IndexOpts;
    IndexOpts.SystemSymbolFilter =
        index::IndexingOptions::SystemSymbolFilterKind::All;
    IndexOpts.IndexFunctionLocals = false;
    Collector = std::make_shared<SymbolCollector>();
    FrontendAction *Action =
        index::createIndexingAction(Collector, IndexOpts, nullptr).release();
    return Action;
  }

  std::shared_ptr<SymbolCollector> Collector;
};

class SymbolCollectorTest : public ::testing::Test {
public:
  bool runSymbolCollector(StringRef HeaderCode, StringRef MainCode) {
    llvm::IntrusiveRefCntPtr<vfs::InMemoryFileSystem> InMemoryFileSystem(
        new vfs::InMemoryFileSystem);
    llvm::IntrusiveRefCntPtr<FileManager> Files(
        new FileManager(FileSystemOptions(), InMemoryFileSystem));

    const std::string FileName = "symbol.cc";
    const std::string HeaderName = "symbols.h";
    auto Factory = llvm::make_unique<SymbolIndexActionFactory>();

    tooling::ToolInvocation Invocation(
        {"symbol_collector", "-fsyntax-only", "-std=c++11", FileName},
        Factory->create(), Files.get(),
        std::make_shared<PCHContainerOperations>());

    InMemoryFileSystem->addFile(HeaderName, 0,
                                llvm::MemoryBuffer::getMemBuffer(HeaderCode));

    std::string Content = "#include\"" + std::string(HeaderName) + "\"";
    Content += "\n" + MainCode.str();
    InMemoryFileSystem->addFile(FileName, 0,
                                llvm::MemoryBuffer::getMemBuffer(Content));
    Invocation.run();
    Symbols = Factory->Collector->takeSymbols();
    return true;
  }

protected:
  SymbolSlab Symbols;
};

TEST_F(SymbolCollectorTest, CollectSymbol) {
  const std::string Header = R"(
    class Foo {
      void f();
    };
    void f1();
    inline void f2() {}
  )";
  const std::string Main = R"(
    namespace {
    void ff() {} // ignore
    }
    void f1() {}
  )";
  runSymbolCollector(Header, Main);
  EXPECT_THAT(Symbols, UnorderedElementsAre(QName("Foo"), QName("Foo::f"),
                                            QName("f1"), QName("f2")));
}

} // namespace
} // namespace clangd
} // namespace clang
