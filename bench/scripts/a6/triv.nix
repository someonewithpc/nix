{ n ? "0" }: derivation { name = "a6-triv-${n}"; system = "x86_64-linux"; builder = "/bin/sh"; args = [ "-c" "echo ${n} > $out" ]; preferLocalBuild = true; allowSubstitutes = false; }
