sh -c 'echo out; echo err >&2' > both 2>&1
sort both
sh -c 'echo err2 >&2' 2>&1 | tr a-z A-Z
