#include "nix/store/derivation/instantiate.hh"
#include "nix/store/derivation/aterm.hh"
#include "nix/store/derivation-options.hh"
#include "nix/store/store-api.hh"
#include "nix/util/hash.hh"

namespace nix::derivation {

Instantiated
instantiate(Store & store, Full & drv, const StructuredAttrsText * structuredAttrs, bool fillIn, bool wantContents)
{
    assert(!(structuredAttrs && drv.structuredAttrs));

    auto inputs = FullInputs::fromSet(drv.inputs);
    auto readDerivation = readInvalid(store);

    std::optional<masked::HashInputs::DrvMap> maskedInputs;

    if (fillIn) {
        /* The checks of `processDerivationOutputPaths`, in its order. */
        auto * options = structuredAttrs ? &structuredAttrs->options : get(drv.structuredAttrs);
        bool rpcOutputs = derivationOptionsFromStructuredAttrs(store, drv.inputs, drv.env, options, false)
                              .requiredSystemFeatures.count(std::string{drvFeatureBuilderRpcV0})
                          != 0;

        auto drvType = type(drv);

        if (rpcOutputs && std::holds_alternative<Type::InputAddressed>(drvType.raw))
            throw Error(
                "derivation uses the '%s' feature, which may only be used with content-addressing derivations",
                drvFeatureBuilderRpcV0);

        for (auto & [outputName, output] : drv.outputs) {
            assert(std::holds_alternative<Output::Deferred>(output.raw));
            assert(getOr(drv.env, outputName, "x") == "");
        }

        maskedInputs = masked::maskInputs(store, readDerivation, inputs, drv.name);
    }

    std::optional<std::string_view> json;
    std::string jsonFromDrv;
    if (structuredAttrs) {
        StructuredAttrs::checkKeyNotInUse(drv.env);
        structuredAttrs->json.check();
        json = structuredAttrs->json.text();
    } else if (drv.structuredAttrs) {
        StructuredAttrs::checkKeyNotInUse(drv.env);
        jsonFromDrv = drv.structuredAttrs->unparse().second;
        json = jsonFromDrv;
    }

    auto tail = unparseTail(store, drv, inputs.srcs, json, fillIn);

    auto envValue = [&](std::string_view name) -> std::string_view { return drv.env.find(std::string{name})->second; };

    if (fillIn && maskedInputs) {
        HashSink sink{HashAlgorithm::SHA256};
        sink(unparseMaskedHead(store, drv, *maskedInputs, /*maskOutputs=*/true));
        tail.write(sink, [](std::string_view) { return std::string_view{}; });
        auto drvHash = sink.finish().hash;

        for (auto & [outputName, output] : drv.outputs) {
            auto outPath = store.makeOutputPath(outputName, drvHash, drv.name);
            output = Output::InputAddressed{.path = outPath};
            drv.env.insert_or_assign(outputName, store.printStorePath(outPath));
        }
    }

    Instantiated res{
        .path = StorePath::dummy,
        .hash = masked::MaskedHash::DeferredDrv{},
    };

    for (auto & input : drv.inputs)
        res.references.insert(input.getBaseStorePath());

    auto head = unparseHead(store, drv, inputs);
    Hash hash(HashAlgorithm::SHA256);
    if (wantContents) {
        StringSink sink;
        sink.s.reserve(head.size() + tail.text.size() + 1024);
        sink(head);
        tail.write(sink, envValue);
        res.contents = std::move(sink.s);
        hash = hashString(HashAlgorithm::SHA256, res.contents);
    } else {
        HashSink sink{HashAlgorithm::SHA256};
        sink(head);
        tail.write(sink, envValue);
        hash = sink.finish().hash;
    }
    res.path = store.makeFixedOutputPathFromCA(
        std::string(drv.name) + drvExtension, TextInfo{.hash = hash, .references = res.references});

    /* As `masked::hashInput`, which would render the input-masked ATerm
       from scratch. */
    if (type(drv).isFixed())
        res.hash = masked::hashInput(store, readDerivation, drv);
    else if (std::ranges::all_of(
                 drv.outputs, [](auto & o) { return std::holds_alternative<Output::InputAddressed>(o.second.raw); })) {
        if (!maskedInputs)
            maskedInputs = masked::maskInputs(store, readDerivation, inputs, drv.name);
        if (maskedInputs) {
            HashSink sink{HashAlgorithm::SHA256};
            sink(unparseMaskedHead(store, drv, *maskedInputs, /*maskOutputs=*/false));
            tail.write(sink, envValue);
            res.hash = sink.finish().hash;
        }
    }

    return res;
}

} // namespace nix::derivation
