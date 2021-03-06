//===-- CodeCompleteTests.cpp -----------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
#include "ClangdServer.h"
#include "Compiler.h"
#include "Context.h"
#include "Matchers.h"
#include "Protocol.h"
#include "TestFS.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace clang {
namespace clangd {
// Let GMock print completion items.
void PrintTo(const CompletionItem &I, std::ostream *O) {
  llvm::raw_os_ostream OS(*O);
  OS << I.label << " - " << toJSON(I);
}
void PrintTo(const std::vector<CompletionItem> &V, std::ostream *O) {
  *O << "{\n";
  for (const auto &I : V) {
    *O << "\t";
    PrintTo(I, O);
    *O << "\n";
  }
  *O << "}";
}

namespace {
using namespace llvm;
using ::testing::AllOf;
using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::Not;

class IgnoreDiagnostics : public DiagnosticsConsumer {
  void onDiagnosticsReady(
      PathRef File, Tagged<std::vector<DiagWithFixIts>> Diagnostics) override {}
};

struct StringWithPos {
  std::string Text;
  clangd::Position MarkerPos;
};

/// Accepts a source file with a cursor marker ^.
/// Returns the source file with the marker removed, and the marker position.
StringWithPos parseTextMarker(StringRef Text) {
  std::size_t MarkerOffset = Text.find('^');
  assert(MarkerOffset != StringRef::npos && "^ wasn't found in Text.");

  std::string WithoutMarker;
  WithoutMarker += Text.take_front(MarkerOffset);
  WithoutMarker += Text.drop_front(MarkerOffset + 1);
  assert(StringRef(WithoutMarker).find('^') == StringRef::npos &&
         "There were multiple occurences of ^ inside Text");

  auto MarkerPos = offsetToPosition(WithoutMarker, MarkerOffset);
  return {std::move(WithoutMarker), MarkerPos};
}

// GMock helpers for matching completion items.
MATCHER_P(Named, Name, "") { return arg.insertText == Name; }
MATCHER_P(Labeled, Label, "") { return arg.label == Label; }
MATCHER_P(Kind, K, "") { return arg.kind == K; }
MATCHER_P(PlainText, Text, "") {
  return arg.insertTextFormat == clangd::InsertTextFormat::PlainText &&
         arg.insertText == Text;
}
MATCHER_P(Snippet, Text, "") {
  return arg.insertTextFormat == clangd::InsertTextFormat::Snippet &&
         arg.insertText == Text;
}
// Shorthand for Contains(Named(Name)).
Matcher<const std::vector<CompletionItem> &> Has(std::string Name) {
  return Contains(Named(std::move(Name)));
}
Matcher<const std::vector<CompletionItem> &> Has(std::string Name,
                                                 CompletionItemKind K) {
  return Contains(AllOf(Named(std::move(Name)), Kind(K)));
}
MATCHER(IsDocumented, "") { return !arg.documentation.empty(); }

CompletionList completions(StringRef Text,
                           clangd::CodeCompleteOptions Opts = {}) {
  MockFSProvider FS;
  MockCompilationDatabase CDB;
  IgnoreDiagnostics DiagConsumer;
  ClangdServer Server(CDB, DiagConsumer, FS, getDefaultAsyncThreadsCount(),
                      /*StorePreamblesInMemory=*/true);
  auto File = getVirtualTestFilePath("foo.cpp");
  auto Test = parseTextMarker(Text);
  Server.addDocument(Context::empty(), File, Test.Text);
  return Server.codeComplete(Context::empty(), File, Test.MarkerPos, Opts)
      .get()
      .second.Value;
}

TEST(CompletionTest, Limit) {
  clangd::CodeCompleteOptions Opts;
  Opts.Limit = 2;
  auto Results = completions(R"cpp(
struct ClassWithMembers {
  int AAA();
  int BBB();
  int CCC();
}
int main() { ClassWithMembers().^ }
      )cpp",
                             Opts);

  EXPECT_TRUE(Results.isIncomplete);
  EXPECT_THAT(Results.items, ElementsAre(Named("AAA"), Named("BBB")));
}

TEST(CompletionTest, Filter) {
  std::string Body = R"cpp(
    int Abracadabra;
    int Alakazam;
    struct S {
      int FooBar;
      int FooBaz;
      int Qux;
    };
  )cpp";
  EXPECT_THAT(completions(Body + "int main() { S().Foba^ }").items,
              AllOf(Has("FooBar"), Has("FooBaz"), Not(Has("Qux"))));

  EXPECT_THAT(completions(Body + "int main() { S().FR^ }").items,
              AllOf(Has("FooBar"), Not(Has("FooBaz")), Not(Has("Qux"))));

  EXPECT_THAT(completions(Body + "int main() { S().opr^ }").items,
              Has("operator="));

  EXPECT_THAT(completions(Body + "int main() { aaa^ }").items,
              AllOf(Has("Abracadabra"), Has("Alakazam")));

  EXPECT_THAT(completions(Body + "int main() { _a^ }").items,
              AllOf(Has("static_cast"), Not(Has("Abracadabra"))));
}

void TestAfterDotCompletion(clangd::CodeCompleteOptions Opts) {
  auto Results = completions(
      R"cpp(
      #define MACRO X

      int global_var;

      int global_func();

      struct GlobalClass {};

      struct ClassWithMembers {
        /// Doc for method.
        int method();

        int field;
      private:
        int private_field;
      };

      int test() {
        struct LocalClass {};

        /// Doc for local_var.
        int local_var;

        ClassWithMembers().^
      }
      )cpp",
      Opts);

  // Class members. The only items that must be present in after-dot
  // completion.
  EXPECT_THAT(
      Results.items,
      AllOf(Has(Opts.EnableSnippets ? "method()" : "method"), Has("field")));
  EXPECT_IFF(Opts.IncludeIneligibleResults, Results.items,
             Has("private_field"));
  // Global items.
  EXPECT_THAT(Results.items, Not(AnyOf(Has("global_var"), Has("global_func"),
                                       Has("global_func()"), Has("GlobalClass"),
                                       Has("MACRO"), Has("LocalClass"))));
  // There should be no code patterns (aka snippets) in after-dot
  // completion. At least there aren't any we're aware of.
  EXPECT_THAT(Results.items, Not(Contains(Kind(CompletionItemKind::Snippet))));
  // Check documentation.
  EXPECT_IFF(Opts.IncludeBriefComments, Results.items,
             Contains(IsDocumented()));
}

void TestGlobalScopeCompletion(clangd::CodeCompleteOptions Opts) {
  auto Results = completions(
      R"cpp(
      #define MACRO X

      int global_var;
      int global_func();

      struct GlobalClass {};

      struct ClassWithMembers {
        /// Doc for method.
        int method();
      };

      int test() {
        struct LocalClass {};

        /// Doc for local_var.
        int local_var;

        ^
      }
      )cpp",
      Opts);

  // Class members. Should never be present in global completions.
  EXPECT_THAT(Results.items,
              Not(AnyOf(Has("method"), Has("method()"), Has("field"))));
  // Global items.
  EXPECT_IFF(Opts.IncludeGlobals, Results.items,
             AllOf(Has("global_var"),
                   Has(Opts.EnableSnippets ? "global_func()" : "global_func"),
                   Has("GlobalClass")));
  // A macro.
  EXPECT_IFF(Opts.IncludeMacros, Results.items, Has("MACRO"));
  // Local items. Must be present always.
  EXPECT_THAT(Results.items,
              AllOf(Has("local_var"), Has("LocalClass"),
                    Contains(Kind(CompletionItemKind::Snippet))));
  // Check documentation.
  EXPECT_IFF(Opts.IncludeBriefComments, Results.items,
             Contains(IsDocumented()));
}

TEST(CompletionTest, CompletionOptions) {
  clangd::CodeCompleteOptions Opts;
  for (bool IncludeMacros : {true, false}) {
    Opts.IncludeMacros = IncludeMacros;
    for (bool IncludeGlobals : {true, false}) {
      Opts.IncludeGlobals = IncludeGlobals;
      for (bool IncludeBriefComments : {true, false}) {
        Opts.IncludeBriefComments = IncludeBriefComments;
        for (bool EnableSnippets : {true, false}) {
          Opts.EnableSnippets = EnableSnippets;
          for (bool IncludeCodePatterns : {true, false}) {
            Opts.IncludeCodePatterns = IncludeCodePatterns;
            for (bool IncludeIneligibleResults : {true, false}) {
              Opts.IncludeIneligibleResults = IncludeIneligibleResults;
              TestAfterDotCompletion(Opts);
              TestGlobalScopeCompletion(Opts);
            }
          }
        }
      }
    }
  }
}

// Check code completion works when the file contents are overridden.
TEST(CompletionTest, CheckContentsOverride) {
  MockFSProvider FS;
  IgnoreDiagnostics DiagConsumer;
  MockCompilationDatabase CDB;
  ClangdServer Server(CDB, DiagConsumer, FS, getDefaultAsyncThreadsCount(),
                      /*StorePreamblesInMemory=*/true);
  auto File = getVirtualTestFilePath("foo.cpp");
  Server.addDocument(Context::empty(), File, "ignored text!");

  auto Example = parseTextMarker("int cbc; int b = ^;");
  auto Results =
      Server
          .codeComplete(Context::empty(), File, Example.MarkerPos,
                        clangd::CodeCompleteOptions(), StringRef(Example.Text))
          .get()
          .second.Value;
  EXPECT_THAT(Results.items, Contains(Named("cbc")));
}

TEST(CompletionTest, Priorities) {
  auto Internal = completions(R"cpp(
      class Foo {
        public: void pub();
        protected: void prot();
        private: void priv();
      };
      void Foo::pub() { this->^ }
  )cpp");
  EXPECT_THAT(Internal.items,
              HasSubsequence(Named("priv"), Named("prot"), Named("pub")));

  auto External = completions(R"cpp(
      class Foo {
        public: void pub();
        protected: void prot();
        private: void priv();
      };
      void test() {
        Foo F;
        F.^
      }
  )cpp");
  EXPECT_THAT(External.items,
              AllOf(Has("pub"), Not(Has("prot")), Not(Has("priv"))));
}

TEST(CompletionTest, Qualifiers) {
  auto Results = completions(R"cpp(
      class Foo {
        public: int foo() const;
        int bar() const;
      };
      class Bar : public Foo {
        int foo() const;
      };
      void test() { Bar().^ }
  )cpp");
  EXPECT_THAT(Results.items, HasSubsequence(Labeled("bar() const"),
                                            Labeled("Foo::foo() const")));
  EXPECT_THAT(Results.items, Not(Contains(Labeled("foo() const")))); // private
}

TEST(CompletionTest, Snippets) {
  clangd::CodeCompleteOptions Opts;
  Opts.EnableSnippets = true;
  auto Results = completions(
      R"cpp(
      struct fake {
        int a;
        int f(int i, const float f) const;
      };
      int main() {
        fake f;
        f.^
      }
      )cpp",
      Opts);
  EXPECT_THAT(Results.items,
              HasSubsequence(PlainText("a"),
                             Snippet("f(${1:int i}, ${2:const float f})")));
}

TEST(CompletionTest, Kinds) {
  auto Results = completions(R"cpp(
      #define MACRO X
      int variable;
      struct Struct {};
      int function();
      int X = ^
  )cpp");
  EXPECT_THAT(Results.items, Has("function", CompletionItemKind::Function));
  EXPECT_THAT(Results.items, Has("variable", CompletionItemKind::Variable));
  EXPECT_THAT(Results.items, Has("int", CompletionItemKind::Keyword));
  EXPECT_THAT(Results.items, Has("Struct", CompletionItemKind::Class));
  EXPECT_THAT(Results.items, Has("MACRO", CompletionItemKind::Text));

  clangd::CodeCompleteOptions Opts;
  Opts.EnableSnippets = true; // Needed for code patterns.

  Results = completions("nam^");
  EXPECT_THAT(Results.items, Has("namespace", CompletionItemKind::Snippet));
}

} // namespace
} // namespace clangd
} // namespace clang
