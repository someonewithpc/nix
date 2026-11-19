#pragma once
///@file

#include "nix/expr/print.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-error.hh"
#include "nix/expr/eval-settings.hh"
#include <exception>

namespace nix {

/**
 * Note: Various places expect the allocated memory to be zeroed.
 */
[[gnu::always_inline]]
inline void * EvalMemory::allocBytes(size_t n)
{
    void * p;
#if NIX_USE_BOEHMGC
    p = GC_MALLOC(n);
#else
    p = calloc(n, 1);
#endif
    if (!p)
        throw std::bad_alloc();
    return p;
}

template<typename T, std::size_t N>
[[gnu::always_inline]]
inline void * EvalMemory::allocBytes(size_t count)
{
    size_t res = 0;

    if (__builtin_mul_overflow(count, sizeof(T), &res)) [[unlikely]]
        throw std::bad_alloc();

    if (__builtin_add_overflow(res, N, &res)) [[unlikely]]
        throw std::bad_alloc();

    return allocBytes(res);
}

[[gnu::always_inline]]
Value * EvalMemory::allocValue()
{
#if NIX_USE_BOEHMGC
    /* GC_malloc_many returns a linked list of objects of the given size,
       where the first word of each object is also the pointer to the next
       object in the list, so that word has to be cleared on the way out */
    void *& list = freeLists[0];
    if (!list) {
        list = GC_malloc_many(sizeof(Value));
        if (!list)
            throw std::bad_alloc();
    }

    void * p = list;
    list = GC_NEXT(p);
    GC_NEXT(p) = nullptr;
#else
    void * p = allocBytes(sizeof(Value));
#endif

    stats.nrValues++;
    return (Value *) p;
}

[[gnu::always_inline]]
Env & EvalMemory::allocEnv(size_t size)
{
    stats.nrEnvs++;
    stats.nrValuesInEnvs += size;

    Env * env;

#if NIX_USE_BOEHMGC
    if (size - 1 < maxListedEnvSize) {
        void *& list = freeLists[size];
        if (!list) {
            list = GC_malloc_many(sizeof(Env) + size * sizeof(Value *));
            if (!list)
                throw std::bad_alloc();
        }

        void * p = list;
        list = GC_NEXT(p);
        GC_NEXT(p) = nullptr;
        env = (Env *) p;
    } else
#endif
        env = (Env *) allocBytes<Value *, sizeof(Env)>(size);

    /* We assume that env->values has been cleared by the allocator; maybeThunk() and lookupVar fromWith expect this. */

    return *env;
}

[[gnu::always_inline]]
void EvalState::forceValue(Value & v, const PosIdx pos)
{
    if (v.isThunk()) {
        Env * env = v.thunk().env;
        assert(env || v.isBlackhole());
        Expr * expr = v.thunk().expr;
        try {
            v.mkBlackhole();
            if (env) [[likely]]
                expr->eval(*this, *env, v);
            else
                ExprBlackHole::throwInfiniteRecursionError(*this, v);
        } catch (...) {
            handleEvalExceptionForThunk(env, expr, v, pos);
            throw;
        }
    } else if (v.isApp()) {
        Value savedApp = v;
        try {
            callFunction(*v.app().left, *v.app().right, v, pos);
        } catch (...) {
            handleEvalExceptionForApp(v, savedApp);
            throw;
        }
    } else if (v.isFailed()) {
        handleEvalFailed(v, pos);
    }
}

[[gnu::always_inline]]
inline void EvalState::forceAttrs(Value & v, const PosIdx pos, std::string_view errorCtx)
{
    forceAttrs(v, [&]() { return pos; }, errorCtx);
}

template<typename Callable>
[[gnu::always_inline]]
inline void EvalState::forceAttrs(Value & v, Callable getPos, std::string_view errorCtx)
{
    PosIdx pos = getPos();
    forceValue(v, pos);
    if (v.type() != nAttrs) {
        error<TypeError>("expected a set but found %1%: %2%", showType(v), ValuePrinter(*this, v, errorPrintOptions))
            .withTrace(pos, errorCtx)
            .debugThrow();
    }
}

[[gnu::always_inline]]
inline void EvalState::forceList(Value & v, const PosIdx pos, std::string_view errorCtx)
{
    forceValue(v, pos);
    if (!v.isList()) {
        error<TypeError>("expected a list but found %1%: %2%", showType(v), ValuePrinter(*this, v, errorPrintOptions))
            .withTrace(pos, errorCtx)
            .debugThrow();
    }
}

[[gnu::always_inline]]
inline CallDepth EvalState::addCallDepth(const PosIdx pos)
{
    if (callDepth > settings.maxCallDepth)
        error<StackOverflowError>().atPos(pos).debugThrow();

    return CallDepth(callDepth);
};

template<typename Cb>
std::invoke_result_t<Cb, Value *, bool>
EvalState::peelToStringOutPath(const PosIdx pos, Value & v, bool checkToStringReturn, Cb && cb)
{
    using R = std::invoke_result_t<Cb, Value *, bool>;
    bool cameThroughToString = false;

    auto peel = [&, &state = *this](this const auto & peel, const PosIdx pos, Value & v) -> R {
        state.forceValue(v, pos);
        auto vType = v.type();
        if (vType != nAttrs) {
            /* Trivially string-coercible types (i.e. coerceMore = false compatible)
               get to return happily.
               String and path terminate happily. External needs to be handled
               by caller. E.g. `printValueAsJSON` routes to `ExternalValueBase::printValueAsJSON`,
               `coerceToString` to `ExternalValueBase::coerceToString`).
               Everything else violates the "`__toString` returns a
               string" contract.
               */
            if (checkToStringReturn && cameThroughToString && vType != nString && vType != nPath && vType != nExternal)
                state
                    .error<TypeError>(
                        "`__toString` should return a string, but got %1%: %2%",
                        showType(v),
                        ValuePrinter(state, v, errorPrintOptions))
                    .atPos(pos)
                    .debugThrow();
            return std::forward<Cb>(cb)(&v, cameThroughToString);
        }
        if (auto i = v.attrs()->get(state.s.toString)) {
            Value & v1 = *state.allocValue();
            try {
                state.callFunction(*i->value, v, v1, i->pos);
            } catch (Error & e) {
                e.addTrace(state.positions[i->pos], "while calling the `__toString` attribute");
                throw;
            }
            cameThroughToString = true;
            try {
                auto _level = state.addCallDepth(pos);
                return peel(i->pos, v1);
            } catch (Error & e) {
                e.addTrace(
                    state.positions[i->pos], "while %s the result of the `__toString` attribute", WhileTryingToUse{v1});
                throw;
            }
        }
        if (auto i = v.attrs()->get(state.s.outPath)) {
            try {
                state.forceValue(*i->value, i->pos);
                auto _level = state.addCallDepth(pos);
                return peel(i->pos, *i->value);
            } catch (Error & e) {
                e.addTrace(state.positions[i->pos], "while %s the `outPath` attribute", WhileTryingToUse{*i->value});
                throw;
            }
        }
        /* Can't peel more. (no `__toString`, no `outPath`).
           If we came through a `__toString` call at any depth, this violates
           the string-return contract. (usually not enforced) */
        if (checkToStringReturn && cameThroughToString)
            state
                .error<TypeError>(
                    "`__toString` must return a string, but got %1%: %2%",
                    showType(v),
                    ValuePrinter(state, v, errorPrintOptions))
                .atPos(pos)
                .debugThrow();
        return std::forward<Cb>(cb)(&v, cameThroughToString);
    };
    return peel(pos, v);
}

} // namespace nix
