//------------------------------------------------------------------------------
// TypeSymbols.cpp
// Contains type-related symbol definitions.
//
// File is under the MIT license; see LICENSE for details.
//------------------------------------------------------------------------------
#include "slang/symbols/TypeSymbols.h"

#include <nlohmann/json.hpp>

#include "slang/binding/ConstantValue.h"
#include "slang/compilation/Compilation.h"
#include "slang/symbols/TypePrinter.h"

namespace {

using namespace slang;

// clang-format off
bitwidth_t getWidth(PredefinedIntegerType::Kind kind) {
    switch (kind) {
        case PredefinedIntegerType::ShortInt: return 16;
        case PredefinedIntegerType::Int: return 32;
        case PredefinedIntegerType::LongInt: return 64;
        case PredefinedIntegerType::Byte: return 8;
        case PredefinedIntegerType::Integer: return 32;
        case PredefinedIntegerType::Time: return 64;
        default: THROW_UNREACHABLE;
    }
}

bool getSigned(PredefinedIntegerType::Kind kind) {
    switch (kind) {
        case PredefinedIntegerType::ShortInt: return true;
        case PredefinedIntegerType::Int: return true;
        case PredefinedIntegerType::LongInt: return true;
        case PredefinedIntegerType::Byte: return true;
        case PredefinedIntegerType::Integer: return true;
        case PredefinedIntegerType::Time: return false;
        default: THROW_UNREACHABLE;
    }
}

bool getFourState(PredefinedIntegerType::Kind kind) {
    switch (kind) {
        case PredefinedIntegerType::ShortInt: return false;
        case PredefinedIntegerType::Int: return false;
        case PredefinedIntegerType::LongInt: return false;
        case PredefinedIntegerType::Byte: return false;
        case PredefinedIntegerType::Integer: return true;
        case PredefinedIntegerType::Time: return true;
        default: THROW_UNREACHABLE;
    }
}
// clang-format on

struct GetDefaultVisitor {
    HAS_METHOD_TRAIT(getDefaultValueImpl);

    template<typename T>
    ConstantValue visit([[maybe_unused]] const T& type) {
        if constexpr (has_getDefaultValueImpl_v<T, ConstantValue>) {
            return type.getDefaultValueImpl();
        }
        else {
            THROW_UNREACHABLE;
        }
    }
};

const Type& getPredefinedType(Compilation& compilation, SyntaxKind kind, bool isSigned) {
    auto& predef = compilation.getType(kind).as<IntegralType>();
    if (isSigned == predef.isSigned)
        return predef;

    auto flags = predef.getIntegralFlags();
    if (isSigned)
        flags |= IntegralFlags::Signed;
    else
        flags &= ~IntegralFlags::Signed;

    return compilation.getType(predef.bitWidth, flags);
}

} // namespace

namespace slang {

const ErrorType ErrorType::Instance;

bitwidth_t Type::getBitWidth() const {
    const Type& ct = getCanonicalType();
    if (ct.isIntegral())
        return ct.as<IntegralType>().bitWidth;

    if (ct.isFloating()) {
        switch (ct.as<FloatingType>().floatKind) {
            case FloatingType::Real:
                return 64;
            case FloatingType::RealTime:
                return 64;
            case FloatingType::ShortReal:
                return 32;
            default:
                THROW_UNREACHABLE;
        }
    }
    return 0;
}

bool Type::isSigned() const {
    const Type& ct = getCanonicalType();
    return ct.isIntegral() && ct.as<IntegralType>().isSigned;
}

bool Type::isFourState() const {
    const Type& ct = getCanonicalType();
    if (ct.isIntegral())
        return ct.as<IntegralType>().isFourState;

    if (ct.kind == SymbolKind::UnpackedArrayType)
        return ct.as<UnpackedArrayType>().elementType.isFourState();

    // TODO: also handle unions
    if (ct.kind == SymbolKind::UnpackedStructType) {
        auto& us = ct.as<UnpackedStructType>();
        for (auto& field : us.membersOfType<FieldSymbol>()) {
            if (field.getType().isFourState())
                return true;
        }
    }

    return false;
}

bool Type::isIntegral() const {
    const Type& ct = getCanonicalType();
    return IntegralType::isKind(ct.kind);
}

bool Type::isAggregate() const {
    switch (getCanonicalType().kind) {
        case SymbolKind::UnpackedArrayType:
        case SymbolKind::UnpackedStructType:
        case SymbolKind::UnpackedUnionType:
            return true;
        default:
            return false;
    }
}

bool Type::isSimpleBitVector() const {
    const Type& ct = getCanonicalType();
    if (ct.isPredefinedInteger() || ct.isScalar())
        return true;

    return ct.kind == SymbolKind::PackedArrayType &&
           ct.as<PackedArrayType>().elementType.isScalar();
}

bool Type::isBooleanConvertible() const {
    switch (getCanonicalType().kind) {
        case SymbolKind::NullType:
        case SymbolKind::CHandleType:
        case SymbolKind::StringType:
        case SymbolKind::EventType:
            return true;
        default:
            return isNumeric();
    }
}

bool Type::isStructUnion() const {
    const Type& ct = getCanonicalType();
    switch (ct.kind) {
        case SymbolKind::PackedStructType:
        case SymbolKind::UnpackedStructType:
        case SymbolKind::PackedUnionType:
        case SymbolKind::UnpackedUnionType:
            return true;
        default:
            return false;
    }
}

bool Type::isMatching(const Type& rhs) const {
    // See [6.22.1] for Matching Types.
    const Type* l = &getCanonicalType();
    const Type* r = &rhs.getCanonicalType();

    // If the two types have the same address, they are literally the same type.
    // This handles all built-in types, which are allocated once and then shared,
    // and also handles simple bit vector types that share the same range, signedness,
    // and four-stateness because we uniquify them in the compilation cache.
    // This handles checks [6.22.1] (a), (b), (c), (d), (g), and (h).
    if (l == r || (l->getSyntax() && l->getSyntax() == r->getSyntax()))
        return true;

    // Special casing for type synonyms: logic/reg
    if (l->isScalar() && r->isScalar()) {
        auto ls = l->as<ScalarType>().scalarKind;
        auto rs = r->as<ScalarType>().scalarKind;
        return (ls == ScalarType::Logic || ls == ScalarType::Reg) &&
               (rs == ScalarType::Logic || rs == ScalarType::Reg);
    }

    // Special casing for type synonyms: real/realtime
    if (l->isFloating() && r->isFloating()) {
        auto lf = l->as<FloatingType>().floatKind;
        auto rf = r->as<FloatingType>().floatKind;
        return (lf == FloatingType::Real || lf == FloatingType::RealTime) &&
               (rf == FloatingType::Real || rf == FloatingType::RealTime);
    }

    // Handle check (e) and (f): matching predefined integers and matching vector types
    if (l->isSimpleBitVector() && r->isSimpleBitVector() &&
        l->isPredefinedInteger() != r->isPredefinedInteger()) {
        auto& li = l->as<IntegralType>();
        auto& ri = r->as<IntegralType>();
        return li.isSigned == ri.isSigned && li.isFourState == ri.isFourState &&
               li.getBitVectorRange() == ri.getBitVectorRange();
    }

    // Handle check (f): matching array types
    if (l->kind == SymbolKind::PackedArrayType && r->kind == SymbolKind::PackedArrayType) {
        auto& la = l->as<PackedArrayType>();
        auto& ra = r->as<PackedArrayType>();
        return la.range == ra.range && la.elementType.isMatching(ra.elementType);
    }
    if (l->kind == SymbolKind::UnpackedArrayType && r->kind == SymbolKind::UnpackedArrayType) {
        auto& la = l->as<UnpackedArrayType>();
        auto& ra = r->as<UnpackedArrayType>();
        return la.range == ra.range && la.elementType.isMatching(ra.elementType);
    }

    return false;
}

bool Type::isEquivalent(const Type& rhs) const {
    // See [6.22.2] for Equivalent Types
    const Type* l = &getCanonicalType();
    const Type* r = &rhs.getCanonicalType();
    if (l->isMatching(*r))
        return true;

    if (l->isIntegral() && r->isIntegral() && !l->isEnum() && !r->isEnum()) {
        const auto& li = l->as<IntegralType>();
        const auto& ri = r->as<IntegralType>();
        return li.isSigned == ri.isSigned && li.isFourState == ri.isFourState &&
               li.bitWidth == ri.bitWidth;
    }

    if (l->kind == SymbolKind::UnpackedArrayType && r->kind == SymbolKind::UnpackedArrayType) {
        auto& la = l->as<UnpackedArrayType>();
        auto& ra = r->as<UnpackedArrayType>();
        return la.range.width() == ra.range.width() && la.elementType.isEquivalent(ra.elementType);
    }

    return false;
}

bool Type::isAssignmentCompatible(const Type& rhs) const {
    // See [6.22.3] for Assignment Compatible
    const Type* l = &getCanonicalType();
    const Type* r = &rhs.getCanonicalType();
    if (l->isEquivalent(*r))
        return true;

    // Any integral or floating value can be implicitly converted to a packed integer
    // value or to a floating value.
    if ((l->isIntegral() && !l->isEnum()) || l->isFloating())
        return r->isIntegral() || r->isFloating();

    return false;
}

bool Type::isCastCompatible(const Type& rhs) const {
    // See [6.22.4] for Cast Compatible
    const Type* l = &getCanonicalType();
    const Type* r = &rhs.getCanonicalType();
    if (l->isAssignmentCompatible(*r))
        return true;

    if (l->isEnum())
        return r->isIntegral() || r->isFloating();

    return false;
}

bitmask<IntegralFlags> Type::getIntegralFlags() const {
    bitmask<IntegralFlags> flags;
    if (!isIntegral())
        return flags;

    const IntegralType& it = getCanonicalType().as<IntegralType>();
    if (it.isSigned)
        flags |= IntegralFlags::Signed;
    if (it.isFourState)
        flags |= IntegralFlags::FourState;
    if (it.isDeclaredReg())
        flags |= IntegralFlags::Reg;

    return flags;
}

ConstantValue Type::getDefaultValue() const {
    GetDefaultVisitor visitor;
    return visit(visitor);
}

ConstantRange Type::getArrayRange() const {
    const Type& t = getCanonicalType();
    if (t.isIntegral())
        return t.as<IntegralType>().getBitVectorRange();

    if (t.isUnpackedArray())
        return t.as<UnpackedArrayType>().range;

    return {};
}

std::string Type::toString() const {
    TypePrinter printer;
    printer.append(*this);
    return printer.toString();
}

const Type& Type::fromSyntax(Compilation& compilation, const DataTypeSyntax& node,
                             LookupLocation location, const Scope& parent, bool forceSigned) {
    switch (node.kind) {
        case SyntaxKind::BitType:
        case SyntaxKind::LogicType:
        case SyntaxKind::RegType:
            return IntegralType::fromSyntax(compilation, node.as<IntegerTypeSyntax>(), location,
                                            parent, forceSigned);
        case SyntaxKind::ByteType:
        case SyntaxKind::ShortIntType:
        case SyntaxKind::IntType:
        case SyntaxKind::LongIntType:
        case SyntaxKind::IntegerType:
        case SyntaxKind::TimeType: {
            auto& its = node.as<IntegerTypeSyntax>();
            if (!its.dimensions.empty()) {
                // Error but don't fail out; just remove the dims and keep trucking
                auto& diag = parent.addDiag(DiagCode::PackedDimsOnPredefinedType,
                                            its.dimensions[0]->openBracket.location());
                diag << getTokenKindText(its.keyword.kind);
            }

            if (!its.signing)
                return compilation.getType(node.kind);

            return getPredefinedType(compilation, node.kind,
                                     its.signing.kind == TokenKind::SignedKeyword);
        }
        case SyntaxKind::RealType:
        case SyntaxKind::RealTimeType:
        case SyntaxKind::ShortRealType:
        case SyntaxKind::StringType:
        case SyntaxKind::CHandleType:
        case SyntaxKind::EventType:
        case SyntaxKind::VoidType:
            return compilation.getType(node.kind);
        case SyntaxKind::EnumType:
            return EnumType::fromSyntax(compilation, node.as<EnumTypeSyntax>(), location, parent,
                                        forceSigned);
        case SyntaxKind::StructType: {
            const auto& structUnion = node.as<StructUnionTypeSyntax>();
            return structUnion.packed ? PackedStructType::fromSyntax(compilation, structUnion,
                                                                     location, parent, forceSigned)
                                      : UnpackedStructType::fromSyntax(compilation, structUnion);
        }
        case SyntaxKind::NamedType:
            return lookupNamedType(compilation, *node.as<NamedTypeSyntax>().name, location, parent);
        case SyntaxKind::ImplicitType: {
            auto& implicit = node.as<ImplicitTypeSyntax>();
            return IntegralType::fromSyntax(
                compilation, SyntaxKind::LogicType, implicit.dimensions,
                implicit.signing.kind == TokenKind::SignedKeyword || forceSigned, location, parent);
        }
        default:
            THROW_UNREACHABLE;
    }
}

bool Type::isKind(SymbolKind kind) {
    switch (kind) {
        case SymbolKind::PredefinedIntegerType:
        case SymbolKind::ScalarType:
        case SymbolKind::FloatingType:
        case SymbolKind::EnumType:
        case SymbolKind::PackedArrayType:
        case SymbolKind::UnpackedArrayType:
        case SymbolKind::PackedStructType:
        case SymbolKind::UnpackedStructType:
        case SymbolKind::PackedUnionType:
        case SymbolKind::UnpackedUnionType:
        case SymbolKind::ClassType:
        case SymbolKind::VoidType:
        case SymbolKind::NullType:
        case SymbolKind::CHandleType:
        case SymbolKind::StringType:
        case SymbolKind::EventType:
        case SymbolKind::TypeAlias:
        case SymbolKind::ErrorType:
            return true;
        default:
            return false;
    }
}

void Type::resolveCanonical() const {
    ASSERT(kind == SymbolKind::TypeAlias);
    canonical = this;
    do {
        canonical = &canonical->as<TypeAliasType>().targetType.getType();
    } while (canonical->isAlias());
}

const Type& Type::lookupNamedType(Compilation& compilation, const NameSyntax& syntax,
                                  LookupLocation location, const Scope& parent) {
    LookupResult result;
    parent.lookupName(syntax, location, LookupFlags::Type, result);

    if (result.hasError())
        compilation.addDiagnostics(result.getDiagnostics());

    return fromLookupResult(compilation, result, syntax, location, parent);
}

const Type& Type::fromLookupResult(Compilation& compilation, const LookupResult& result,
                                   const NameSyntax& syntax, LookupLocation location,
                                   const Scope& parent) {
    const Symbol* symbol = result.found;
    if (!symbol)
        return compilation.getErrorType();

    if (!symbol->isType()) {
        parent.addDiag(DiagCode::NotAType, syntax.sourceRange()) << symbol->name;
        return compilation.getErrorType();
    }

    BindContext context(parent, location);

    const Type* finalType = &symbol->as<Type>();
    uint32_t count = result.selectors.size();
    for (uint32_t i = 0; i < count; i++) {
        // TODO: handle dotted selectors
        auto selectSyntax = std::get<const ElementSelectSyntax*>(result.selectors[count - i - 1]);
        auto dim = context.evalPackedDimension(*selectSyntax);
        if (!dim)
            return compilation.getErrorType();

        finalType = &PackedArrayType::fromSyntax(compilation, *finalType, *dim, *selectSyntax);
    }

    return *finalType;
}

IntegralType::IntegralType(SymbolKind kind, string_view name, SourceLocation loc,
                           bitwidth_t bitWidth_, bool isSigned_, bool isFourState_) :
    Type(kind, name, loc),
    bitWidth(bitWidth_), isSigned(isSigned_), isFourState(isFourState_) {
}

bool IntegralType::isKind(SymbolKind kind) {
    switch (kind) {
        case SymbolKind::PredefinedIntegerType:
        case SymbolKind::ScalarType:
        case SymbolKind::EnumType:
        case SymbolKind::PackedArrayType:
        case SymbolKind::PackedStructType:
        case SymbolKind::PackedUnionType:
            return true;
        default:
            return false;
    }
}

ConstantRange IntegralType::getBitVectorRange() const {
    if (isPredefinedInteger() || isScalar() || kind == SymbolKind::PackedStructType ||
        kind == SymbolKind::PackedUnionType) {

        return { int32_t(bitWidth - 1), 0 };
    }

    return as<PackedArrayType>().range;
}

bool IntegralType::isDeclaredReg() const {
    const Type* type = this;
    while (type->kind == SymbolKind::PackedArrayType)
        type = &type->as<PackedArrayType>().elementType.getCanonicalType();

    if (type->isScalar())
        return type->as<ScalarType>().scalarKind == ScalarType::Reg;

    return false;
}

const Type& IntegralType::fromSyntax(Compilation& compilation, SyntaxKind integerKind,
                                     span<const VariableDimensionSyntax* const> dimensions,
                                     bool isSigned, LookupLocation location, const Scope& scope) {
    // This is a simple integral vector (possibly of just one element).
    BindContext context(scope, location);
    SmallVectorSized<std::pair<ConstantRange, const SyntaxNode*>, 4> dims;
    for (auto dimSyntax : dimensions) {
        auto dim = context.evalPackedDimension(*dimSyntax);
        if (!dim)
            return compilation.getErrorType();

        dims.emplace(*dim, dimSyntax);
    }

    if (dims.empty())
        return getPredefinedType(compilation, integerKind, isSigned);

    bitmask<IntegralFlags> flags;
    if (integerKind == SyntaxKind::RegType)
        flags |= IntegralFlags::Reg;
    if (isSigned)
        flags |= IntegralFlags::Signed;
    if (integerKind != SyntaxKind::BitType)
        flags |= IntegralFlags::FourState;

    if (dims.size() == 1 && dims[0].first.right == 0) {
        // if we have the common case of only one dimension and lsb == 0
        // then we can use the shared representation
        return compilation.getType(dims[0].first.width(), flags);
    }

    const Type* result = &compilation.getScalarType(flags);
    uint32_t count = dims.size();
    for (uint32_t i = 0; i < count; i++) {
        auto& pair = dims[count - i - 1];
        result = &PackedArrayType::fromSyntax(compilation, *result, pair.first, *pair.second);
    }

    return *result;
}

const Type& IntegralType::fromSyntax(Compilation& compilation, const IntegerTypeSyntax& syntax,
                                     LookupLocation location, const Scope& scope,
                                     bool forceSigned) {
    return fromSyntax(compilation, syntax.kind, syntax.dimensions,
                      syntax.signing.kind == TokenKind::SignedKeyword || forceSigned, location,
                      scope);
}

ConstantValue IntegralType::getDefaultValueImpl() const {
    if (isEnum())
        return as<EnumType>().baseType.getDefaultValue();

    if (isFourState)
        return SVInt::createFillX(bitWidth, isSigned);
    else
        return SVInt(bitWidth, 0, isSigned);
}

PredefinedIntegerType::PredefinedIntegerType(Kind integerKind) :
    PredefinedIntegerType(integerKind, getSigned(integerKind)) {
}

PredefinedIntegerType::PredefinedIntegerType(Kind integerKind, bool isSigned) :
    IntegralType(SymbolKind::PredefinedIntegerType, "", SourceLocation(), getWidth(integerKind),
                 isSigned, getFourState(integerKind)),
    integerKind(integerKind) {
}

bool PredefinedIntegerType::isDefaultSigned(Kind integerKind) {
    return getSigned(integerKind);
}

ScalarType::ScalarType(Kind scalarKind) : ScalarType(scalarKind, false) {
}

ScalarType::ScalarType(Kind scalarKind, bool isSigned) :
    IntegralType(SymbolKind::ScalarType, "", SourceLocation(), 1, isSigned,
                 scalarKind != Kind::Bit),
    scalarKind(scalarKind) {
}

FloatingType::FloatingType(Kind floatKind_) :
    Type(SymbolKind::FloatingType, "", SourceLocation()), floatKind(floatKind_) {
}

ConstantValue FloatingType::getDefaultValueImpl() const {
    return 0.0;
}

EnumType::EnumType(Compilation& compilation, SourceLocation loc, const Type& baseType_,
                   const Scope& scope) :
    IntegralType(SymbolKind::EnumType, "", loc, baseType_.getBitWidth(), baseType_.isSigned(),
                 baseType_.isFourState()),
    Scope(compilation, this), baseType(baseType_) {

    // Enum types don't live as members of the parent scope (they're "owned" by the declaration
    // containing them) but we hook up the parent pointer so that it can participate in name
    // lookups.
    setParent(scope);
}

const Type& EnumType::fromSyntax(Compilation& compilation, const EnumTypeSyntax& syntax,
                                 LookupLocation location, const Scope& scope, bool forceSigned) {
    const Type* base;
    const Type* canonicalBase;
    if (!syntax.baseType) {
        base = &compilation.getIntType();
        canonicalBase = base;
    }
    else {
        base = &compilation.getType(*syntax.baseType, location, scope, forceSigned);

        canonicalBase = &base->getCanonicalType();
        if (canonicalBase->isError())
            return *canonicalBase;

        // TODO: better checking of enum base types
        if (!canonicalBase->isSimpleBitVector()) {
            scope.addDiag(DiagCode::InvalidEnumBase, syntax.baseType->getFirstToken().location())
                << *base;
            return compilation.getErrorType();
        }
    }

    auto resultType =
        compilation.emplace<EnumType>(compilation, syntax.keyword.location(), *base, scope);
    resultType->setSyntax(syntax);

    SVInt one(canonicalBase->getBitWidth(), 1, canonicalBase->isSigned());
    SVInt current(canonicalBase->getBitWidth(), 0, canonicalBase->isSigned());

    // TODO: error if no members
    for (auto member : syntax.members) {
        auto ev =
            compilation.emplace<EnumValueSymbol>(member->name.valueText(), member->name.location());
        ev->setType(*resultType);
        ev->setSyntax(*member);
        resultType->addMember(*ev);

        if (!member->initializer) {
            ev->setValue(current);
            current += one;
        }
        else {
            // TODO: require integer in binding
            ev->setInitializerSyntax(*member->initializer->expr,
                                     member->initializer->equals.location());
            if (auto& cv = ev->getConstantValue())
                current = cv.integer() + one;
            else
                current += one;
        }
    }

    return *resultType;
}

EnumValueSymbol::EnumValueSymbol(string_view name, SourceLocation loc) :
    ValueSymbol(SymbolKind::EnumValue, name, loc, DeclaredTypeFlags::RequireConstant) {
}

const ConstantValue& EnumValueSymbol::getValue() const {
    return value ? *value : getConstantValue();
}

void EnumValueSymbol::setValue(ConstantValue newValue) {
    auto scope = getScope();
    ASSERT(scope);
    value = scope->getCompilation().allocConstant(std::move(newValue));
}

void EnumValueSymbol::toJson(json& j) const {
    if (value)
        j["value"] = *value;
}

PackedArrayType::PackedArrayType(const Type& elementType, ConstantRange range) :
    IntegralType(SymbolKind::PackedArrayType, "", SourceLocation(),
                 elementType.getBitWidth() * range.width(), elementType.isSigned(),
                 elementType.isFourState()),
    elementType(elementType), range(range) {
}

const Type& PackedArrayType::fromSyntax(Compilation& compilation, const Type& elementType,
                                        ConstantRange range, const SyntaxNode& syntax) {
    if (elementType.isError())
        return elementType;

    // TODO: check bitwidth of array
    auto result = compilation.emplace<PackedArrayType>(elementType, range);
    result->setSyntax(syntax);
    return *result;
}

UnpackedArrayType::UnpackedArrayType(const Type& elementType, ConstantRange range) :
    Type(SymbolKind::UnpackedArrayType, "", SourceLocation()), elementType(elementType),
    range(range) {
}

const Type& UnpackedArrayType::fromSyntax(Compilation& compilation, const Type& elementType,
                                          LookupLocation location, const Scope& scope,
                                          const SyntaxList<VariableDimensionSyntax>& dimensions) {
    if (elementType.isError())
        return elementType;

    BindContext context(scope, location);

    const Type* result = &elementType;
    uint32_t count = (uint32_t)dimensions.size();
    for (uint32_t i = 0; i < count; i++) {
        // TODO: handle other kinds of unpacked arrays
        EvaluatedDimension dim = context.evalDimension(*dimensions[count - i - 1], true);
        if (!dim.isRange())
            return compilation.getErrorType();

        auto unpacked = compilation.emplace<UnpackedArrayType>(*result, dim.range);
        unpacked->setSyntax(*dimensions[count - i - 1]);
        result = unpacked;
    }

    return *result;
}

ConstantValue UnpackedArrayType::getDefaultValueImpl() const {
    // TODO: implement this
    THROW_UNREACHABLE;
}

bool FieldSymbol::isPacked() const {
    const Scope* scope = getScope();
    ASSERT(scope);
    return scope->asSymbol().kind == SymbolKind::PackedStructType ||
           scope->asSymbol().kind == SymbolKind::UnpackedStructType;
}

void FieldSymbol::toJson(json& j) const {
    VariableSymbol::toJson(j);
    j["offset"] = offset;
}

PackedStructType::PackedStructType(Compilation& compilation, bitwidth_t bitWidth, bool isSigned,
                                   bool isFourState) :
    IntegralType(SymbolKind::PackedStructType, "", SourceLocation(), bitWidth, isSigned,
                 isFourState),
    Scope(compilation, this) {
}

const Type& PackedStructType::fromSyntax(Compilation& compilation,
                                         const StructUnionTypeSyntax& syntax,
                                         LookupLocation location, const Scope& scope,
                                         bool forceSigned) {
    ASSERT(syntax.packed);
    bool isSigned = syntax.signing.kind == TokenKind::SignedKeyword || forceSigned;
    bool isFourState = false;
    bitwidth_t bitWidth = 0;

    // We have to look at all the members up front to know our width and four-statedness.
    // We have to iterate in reverse because members are specified from MSB to LSB order.
    SmallVectorSized<const Symbol*, 8> members;
    for (auto member : make_reverse_range(syntax.members)) {
        const Type& type = compilation.getType(*member->type, location, scope);
        isFourState |= type.isFourState();

        bool issuedError = false;
        if (!type.isIntegral() && !type.isError()) {
            issuedError = true;
            auto& diag = scope.addDiag(DiagCode::PackedMemberNotIntegral,
                                       member->type->getFirstToken().location());
            diag << type;
            diag << member->type->sourceRange();
        }

        for (auto decl : member->declarators) {
            auto variable = compilation.emplace<FieldSymbol>(decl->name.valueText(),
                                                             decl->name.location(), bitWidth);
            variable->setType(type);
            variable->setSyntax(*decl);
            compilation.addAttributes(*variable, member->attributes);
            members.append(variable);

            // Unpacked arrays are disallowed in packed structs.
            if (const Type& dimType = compilation.getType(type, decl->dimensions, location, scope);
                dimType.isUnpackedArray() && !issuedError) {

                auto& diag = scope.addDiag(DiagCode::PackedMemberNotIntegral, decl->name.range());
                diag << dimType;
                diag << decl->dimensions.sourceRange();
            }

            bitWidth += type.getBitWidth();

            if (decl->initializer) {
                auto& diag = scope.addDiag(DiagCode::PackedMemberHasInitializer,
                                           decl->initializer->equals.location());
                diag << decl->initializer->expr->sourceRange();
            }
        }
    }

    auto structType =
        compilation.emplace<PackedStructType>(compilation, bitWidth, isSigned, isFourState);
    for (auto member : make_reverse_range(members))
        structType->addMember(*member);

    structType->setSyntax(syntax);

    const Type* result = structType;
    BindContext context(scope, location);

    ptrdiff_t count = syntax.dimensions.size();
    for (ptrdiff_t i = 0; i < count; i++) {
        auto& dimSyntax = *syntax.dimensions[count - i - 1];
        auto dim = context.evalPackedDimension(dimSyntax);
        if (!dim)
            return compilation.getErrorType();

        result = &PackedArrayType::fromSyntax(compilation, *result, *dim, dimSyntax);
    }

    return *result;
}

UnpackedStructType::UnpackedStructType(Compilation& compilation) :
    Type(SymbolKind::UnpackedStructType, "", SourceLocation()), Scope(compilation, this) {
}

ConstantValue UnpackedStructType::getDefaultValueImpl() const {
    // TODO: implement this
    THROW_UNREACHABLE;
}

const Type& UnpackedStructType::fromSyntax(Compilation& compilation,
                                           const StructUnionTypeSyntax& syntax) {
    ASSERT(!syntax.packed);

    uint32_t fieldIndex = 0;
    auto result = compilation.emplace<UnpackedStructType>(compilation);
    for (auto member : syntax.members) {
        for (auto decl : member->declarators) {
            auto variable = compilation.emplace<FieldSymbol>(decl->name.valueText(),
                                                             decl->name.location(), fieldIndex);
            variable->setDeclaredType(*member->type);
            variable->setFromDeclarator(*decl);
            compilation.addAttributes(*variable, member->attributes);

            result->addMember(*variable);
            fieldIndex++;
        }
    }

    result->setSyntax(syntax);
    return *result;
}

ConstantValue NullType::getDefaultValueImpl() const {
    return ConstantValue::NullPlaceholder{};
}

ConstantValue CHandleType::getDefaultValueImpl() const {
    return ConstantValue::NullPlaceholder{};
}

ConstantValue StringType::getDefaultValueImpl() const {
    // TODO: implement this
    THROW_UNREACHABLE;
}

ConstantValue EventType::getDefaultValueImpl() const {
    return ConstantValue::NullPlaceholder{};
}

const ForwardingTypedefSymbol& ForwardingTypedefSymbol::fromSyntax(
    Compilation& compilation, const ForwardTypedefDeclarationSyntax& syntax) {

    Category category;
    switch (syntax.keyword.kind) {
        case TokenKind::EnumKeyword:
            category = Category::Enum;
            break;
        case TokenKind::StructKeyword:
            category = Category::Struct;
            break;
        case TokenKind::UnionKeyword:
            category = Category::Union;
            break;
        case TokenKind::ClassKeyword:
            category = Category::Class;
            break;
        default:
            category = Category::None;
            break;
    }
    auto result = compilation.emplace<ForwardingTypedefSymbol>(syntax.name.valueText(),
                                                               syntax.name.location(), category);
    result->setSyntax(syntax);
    compilation.addAttributes(*result, syntax.attributes);
    return *result;
}

const ForwardingTypedefSymbol& ForwardingTypedefSymbol::fromSyntax(
    Compilation& compilation, const ForwardInterfaceClassTypedefDeclarationSyntax& syntax) {

    auto result = compilation.emplace<ForwardingTypedefSymbol>(
        syntax.name.valueText(), syntax.name.location(), Category::InterfaceClass);
    result->setSyntax(syntax);
    compilation.addAttributes(*result, syntax.attributes);
    return *result;
}

void ForwardingTypedefSymbol::addForwardDecl(const ForwardingTypedefSymbol& decl) const {
    if (!next)
        next = &decl;
    else
        next->addForwardDecl(decl);
}

void ForwardingTypedefSymbol::toJson(json& j) const {
    j["category"] = toString(category);
    if (next)
        j["next"] = *next;
}

const TypeAliasType& TypeAliasType::fromSyntax(Compilation& compilation,
                                               const TypedefDeclarationSyntax& syntax) {
    // TODO: unpacked dimensions
    auto result =
        compilation.emplace<TypeAliasType>(syntax.name.valueText(), syntax.name.location());
    result->targetType.setTypeSyntax(*syntax.type);
    result->setSyntax(syntax);
    compilation.addAttributes(*result, syntax.attributes);
    return *result;
}

void TypeAliasType::addForwardDecl(const ForwardingTypedefSymbol& decl) const {
    if (!firstForward)
        firstForward = &decl;
    else
        firstForward->addForwardDecl(decl);
}

void TypeAliasType::checkForwardDecls() const {
    ForwardingTypedefSymbol::Category category;
    switch (targetType.getType().kind) {
        case SymbolKind::PackedStructType:
        case SymbolKind::UnpackedStructType:
            category = ForwardingTypedefSymbol::Struct;
            break;
        case SymbolKind::EnumType:
            category = ForwardingTypedefSymbol::Enum;
            break;
        default:
            return;
    }

    const ForwardingTypedefSymbol* forward = firstForward;
    while (forward) {
        if (forward->category != ForwardingTypedefSymbol::None && forward->category != category) {
            auto& diag =
                getScope()->addDiag(DiagCode::ForwardTypedefDoesNotMatch, forward->location);
            switch (forward->category) {
                case ForwardingTypedefSymbol::Enum:
                    diag << "enum";
                    break;
                case ForwardingTypedefSymbol::Struct:
                    diag << "struct";
                    break;
                case ForwardingTypedefSymbol::Union:
                    diag << "union";
                    break;
                case ForwardingTypedefSymbol::Class:
                    diag << "class";
                    break;
                case ForwardingTypedefSymbol::InterfaceClass:
                    diag << "interface class";
                    break;
                default:
                    THROW_UNREACHABLE;
            }
            diag.addNote(DiagCode::NoteDeclarationHere, location);
            return;
        }
        forward = forward->getNextForwardDecl();
    }
}

ConstantValue TypeAliasType::getDefaultValueImpl() const {
    return targetType.getType().getDefaultValue();
}

void TypeAliasType::toJson(json& j) const {
    j["target"] = targetType.getType();
    if (firstForward)
        j["forward"] = *firstForward;
}

NetType::NetType(NetKind netKind, string_view name, const Type& dataType) :
    Symbol(SymbolKind::NetType, name, SourceLocation()), netKind(netKind), declaredType(*this),
    isResolved(true) {

    declaredType.setType(dataType);
}

NetType::NetType(string_view name, SourceLocation location) :
    Symbol(SymbolKind::NetType, name, location), netKind(UserDefined), declaredType(*this) {
}

const NetType* NetType::getAliasTarget() const {
    if (!isResolved)
        resolve();
    return alias;
}

const NetType& NetType::getCanonical() const {
    if (auto target = getAliasTarget())
        return target->getCanonical();
    return *this;
}

const Type& NetType::getDataType() const {
    if (!isResolved)
        resolve();
    return declaredType.getType();
}

const SubroutineSymbol* NetType::getResolutionFunction() const {
    if (!isResolved)
        resolve();
    return resolver;
}

void NetType::toJson(json& j) const {
    j["type"] = getDataType();
    if (auto target = getAliasTarget())
        j["target"] = *target;
}

NetType& NetType::fromSyntax(Compilation& compilation, const NetTypeDeclarationSyntax& syntax) {
    auto result = compilation.emplace<NetType>(syntax.name.valueText(), syntax.name.location());
    result->setSyntax(syntax);
    compilation.addAttributes(*result, syntax.attributes);

    // If this is an enum, make sure the declared type is set up before we get added to
    // any scope, so that the enum members get picked up correctly.
    if (syntax.type->kind == SyntaxKind::EnumType)
        result->declaredType.setTypeSyntax(*syntax.type);

    return *result;
}

void NetType::resolve() const {
    ASSERT(!isResolved);
    isResolved = true;

    auto syntaxNode = getSyntax();
    ASSERT(syntaxNode);

    auto scope = getScope();
    ASSERT(scope);

    auto& declSyntax = syntaxNode->as<NetTypeDeclarationSyntax>();
    if (declSyntax.withFunction) {
        // TODO: lookup and validate the function here
    }

    // If this is an enum, we already set the type earlier.
    if (declSyntax.type->kind == SyntaxKind::EnumType)
        return;

    // Our type syntax is either a link to another net type we are aliasing, or an actual
    // data type that we are using as the basis for a custom net type.
    if (declSyntax.type->kind == SyntaxKind::NamedType) {
        LookupResult result;
        const NameSyntax& nameSyntax = *declSyntax.type->as<NamedTypeSyntax>().name;
        scope->lookupName(nameSyntax, LookupLocation::before(*this), LookupFlags::Type, result);

        if (result.found && result.found->kind == SymbolKind::NetType) {
            if (result.hasError())
                scope->getCompilation().addDiagnostics(result.getDiagnostics());

            alias = &result.found->as<NetType>();
            declaredType.copyTypeFrom(alias->getCanonical().declaredType);
            return;
        }
    }

    declaredType.setTypeSyntax(*declSyntax.type);
}

} // namespace slang
