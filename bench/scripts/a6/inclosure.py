import json, subprocess, sys
for d in sys.argv[1:]:
    j = json.loads(subprocess.run(["nix","derivation","show",d],capture_output=True,text=True).stdout)
    j = j.get("derivations", j); k, v = next(iter(j.items()))
    ins = v.get("inputs", {})
    srcs = ins.get("srcs", v.get("inputSrcs", []))
    drvs = ins.get("drvs", v.get("inputDrvs", {}))
    paths = ["/nix/store/" + s if not s.startswith("/") else s for s in srcs]
    for dp, o in drvs.items():
        dp = dp if dp.startswith("/") else "/nix/store/" + dp
        outs = o["outputs"] if isinstance(o, dict) else o
        oj = json.loads(subprocess.run(["nix","derivation","show",dp],capture_output=True,text=True).stdout)
        oj = oj.get("derivations", oj); _, ov = next(iter(oj.items()))
        for name in outs:
            p = ov["outputs"][name].get("path")
            if p: paths.append(p if p.startswith("/") else "/nix/store/" + p)
    valid = [p for p in paths if subprocess.run(["nix-store","--check-validity",p],capture_output=True).returncode == 0]
    r = subprocess.run(["nix-store","-qR",*valid],capture_output=True,text=True) if valid else None
    print("%-45s %5d input closure paths, %d direct inputs not built yet" % (v["name"][:45], len(r.stdout.split()) if r else 0, len(paths)-len(valid)))
