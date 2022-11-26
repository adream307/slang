// SPDX-FileCopyrightText: Michael Popoloski
// SPDX-License-Identifier: MIT

#include "Test.h"

TEST_CASE("Diagnose unused modules / interfaces") {
    auto tree = SyntaxTree::fromText(R"(
interface I;
endinterface

interface J;
endinterface

module bar (I i);
endmodule

module top;
endmodule

module top2({a[1:0], a[3:2]});
    ref int a;
endmodule

module top3(ref int a);
endmodule
)");

    CompilationOptions coptions;
    coptions.suppressUnused = false;

    Compilation compilation(coptions);
    compilation.addSyntaxTree(tree);

    auto& diags = compilation.getAllDiagnostics();
    REQUIRE(diags.size() == 5);
    CHECK(diags[0].code == diag::UnusedDefinition);
    CHECK(diags[1].code == diag::TopModuleIfacePort);
    CHECK(diags[2].code == diag::TopModuleUnnamedRefPort);
    CHECK(diags[3].code == diag::TopModuleRefPort);
    CHECK(diags[4].code == diag::UnusedPort);
}

TEST_CASE("Unused nets and vars") {
    auto tree = SyntaxTree::fromText(R"(
module m #(int foo)(input baz, output bar);
    int i;
    if (foo > 1) assign i = 0;

    int x = 1;
    int z;
    int y = x + z;

    wire j = 1;
    wire k;
    wire l = k;
    wire m;

    assign m = 1;
endmodule

module top;
    logic baz,bar;
    m #(1) m1(.*);
    m #(2) m2(bar, baz);
endmodule
)");

    CompilationOptions coptions;
    coptions.suppressUnused = false;

    Compilation compilation(coptions);
    compilation.addSyntaxTree(tree);

    auto& diags = compilation.getAllDiagnostics();
    REQUIRE(diags.size() == 9);
    CHECK(diags[0].code == diag::UnusedPort);
    CHECK(diags[1].code == diag::UndrivenPort);
    CHECK(diags[2].code == diag::UnusedButSetVariable);
    CHECK(diags[3].code == diag::UnassignedVariable);
    CHECK(diags[4].code == diag::UnusedVariable);
    CHECK(diags[5].code == diag::UnusedNet);
    CHECK(diags[6].code == diag::UndrivenNet);
    CHECK(diags[7].code == diag::UnusedNet);
    CHECK(diags[8].code == diag::UnusedButSetNet);
}
