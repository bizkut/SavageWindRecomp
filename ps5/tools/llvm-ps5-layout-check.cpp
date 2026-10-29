// Compile-time guards for the PS5 LLVM/clang trailing-object allocation ABI.
// SPDX-License-Identifier: GPL-3.0-or-later
// This is compiled with -fno-access-control to inspect LLVM's private ExtraInfo.
#include <llvm/CodeGen/MachineInstr.h>
#include <llvm/IR/DataLayout.h>
#include <clang/AST/DeclGroup.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/Stmt.h>
#include <clang/Sema/ParsedAttr.h>
#include <array>
#include <cstddef>
#include <utility>

// Check the same alignment transitions as TrailingObjects' accessors at every
// possible base address residue, with full and alternating empty arrays.
template <size_t Variant, size_t Index>
constexpr size_t count() {
    return Variant == 0 ? 1 : Variant == 1 ? 2 : ((Index + Variant) % 2) * 2;
}
template <size_t Variant, class Base, class... Types, size_t... I>
constexpr bool fits_variant(std::index_sequence<I...>)
{
    constexpr std::array<size_t, sizeof...(Types)> alignments{alignof(Types)...};
    constexpr std::array<size_t, sizeof...(Types)> sizes{sizeof(Types)...};
    constexpr std::array<size_t, sizeof...(Types)> counts{count<Variant, I>()...};
    constexpr size_t allocated = llvm::TrailingObjects<Base, Types...>::template
        totalSizeToAlloc<Types...>(count<Variant, I>()...);
    constexpr size_t additional = llvm::TrailingObjects<Base, Types...>::template
        additionalSizeToAlloc<Types...>(count<Variant, I>()...);
    if (allocated != sizeof(Base) + additional)
        return false;
    size_t maximum_alignment = alignof(Base);
    for (size_t alignment : alignments)
        if (alignment > maximum_alignment)
            maximum_alignment = alignment;
    for (size_t residue = 0; residue < maximum_alignment; residue += alignof(Base)) {
        size_t position = residue + sizeof(Base);
        size_t previous_alignment = alignof(Base);
        for (size_t i = 0; i < alignments.size(); ++i) {
            if (previous_alignment < alignments[i])
                position = (position + alignments[i] - 1) & ~(alignments[i] - 1);
            position += counts[i] * sizes[i];
            if (counts[i] && position - residue > allocated)
                return false;
            previous_alignment = alignments[i];
        }
    }
    return true;
}

template <class Base, class... Types>
constexpr bool fits() {
    using Index = std::index_sequence_for<Types...>;
    return fits_variant<0, Base, Types...>(Index{}) && fits_variant<1, Base, Types...>(Index{}) &&
           fits_variant<2, Base, Types...>(Index{}) && fits_variant<3, Base, Types...>(Index{});
}

// Real public-header users observed in the LLVM/clang 18.1.8 census, including
// the private MachineInstr type that failed on the console.
static_assert(fits<llvm::MachineInstr::ExtraInfo, llvm::MachineMemOperand *, llvm::MCSymbol *,
                   llvm::MDNode *, uint32_t>());
static_assert(fits<llvm::StructLayout, llvm::TypeSize>());
static_assert(fits<clang::DeclGroup, clang::Decl *>());
static_assert(fits<clang::TemplateParameterList, clang::NamedDecl *, clang::Expr *>());
static_assert(fits<clang::CompoundStmt, clang::Stmt *, clang::FPOptionsOverride>());
static_assert(fits<clang::CaseStmt, clang::Stmt *, clang::SourceLocation>());
static_assert(fits<clang::ConstantExpr, clang::APValue, uint64_t>());
static_assert(fits<clang::DeclRefExpr, clang::NestedNameSpecifierLoc, clang::NamedDecl *,
                   clang::ASTTemplateKWAndArgsInfo, clang::TemplateArgumentLoc>());
static_assert(fits<clang::StringLiteral, unsigned, clang::SourceLocation, char>());
static_assert(fits<clang::MemberExpr, clang::MemberExprNameQualifier, clang::ASTTemplateKWAndArgsInfo,
                   clang::TemplateArgumentLoc>());
static_assert(fits<clang::PragmaCommentDecl, char>());
static_assert(fits<clang::PragmaDetectMismatchDecl, char>());
static_assert(fits<clang::CapturedDecl, clang::ImplicitParamDecl *>());
static_assert(fits<clang::ImportDecl, clang::SourceLocation>());
static_assert(fits<clang::ConstantArrayType, const clang::Expr *>());
static_assert(fits<clang::UsingType, clang::QualType>());
static_assert(fits<clang::FunctionProtoType, clang::QualType, clang::SourceLocation,
                   clang::FunctionType::FunctionTypeExtraBitfields, clang::FunctionType::FunctionTypeArmAttributes,
                   clang::FunctionType::ExceptionType, clang::Expr *, clang::FunctionDecl *,
                   clang::FunctionType::ExtParameterInfo, clang::Qualifiers>());
static_assert(fits<clang::ParsedAttr, clang::ArgsUnion, clang::detail::AvailabilityData,
                   clang::detail::TypeTagForDatatypeData, clang::ParsedType, clang::detail::PropertyData>());
static_assert(fits<clang::PredefinedExpr, clang::Stmt *>());
static_assert(fits<clang::UnaryOperator, clang::FPOptionsOverride>());
static_assert(fits<clang::OffsetOfExpr, clang::OffsetOfNode, clang::Expr *>());
static_assert(fits<clang::ImplicitCastExpr, clang::CXXBaseSpecifier *, clang::FPOptionsOverride>());
static_assert(fits<clang::CStyleCastExpr, clang::CXXBaseSpecifier *, clang::FPOptionsOverride>());
static_assert(fits<clang::DesignatedInitExpr, clang::Stmt *>());
static_assert(fits<clang::ParenListExpr, clang::Stmt *>());
static_assert(fits<clang::GenericSelectionExpr, clang::Stmt *, clang::TypeSourceInfo *>());
static_assert(fits<clang::PseudoObjectExpr, clang::Expr *>());
static_assert(fits<clang::RecoveryExpr, clang::Expr *>());
static_assert(fits<clang::TypedefType, clang::QualType>());
static_assert(fits<clang::SubstTemplateTypeParmType, clang::QualType>());
static_assert(fits<clang::ElaboratedType, clang::TagDecl *>());

// Other manually tail-allocated layouts found by the base-relative-offset audit.
static_assert(alignof(clang::CapturedStmt) >= alignof(clang::Stmt *));
static_assert(alignof(clang::CXXConstructExpr) >= alignof(clang::Stmt *));
static_assert(alignof(clang::CXXTemporaryObjectExpr) >= alignof(clang::Stmt *));
static_assert(alignof(clang::OverloadedTemplateStorage) >= alignof(clang::NamedDecl *));
static_assert(alignof(clang::AutoType) >= alignof(clang::TemplateArgument));
static_assert(alignof(clang::TemplateSpecializationType) >= alignof(clang::TemplateArgument));
static_assert(alignof(clang::DependentTemplateSpecializationType) >= alignof(clang::TemplateArgument));

// Reproduce the console's 12-byte/4-aligned base shape independently of LLVM
// internals. The target ABI may differ from host ABI; both must satisfy fits.
class Probe final : public llvm::TrailingObjects<Probe, void *, unsigned> {
    int count;
    bool flags[5];
};
static_assert(fits<Probe, void *, unsigned>());
