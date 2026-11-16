true && echo and-yes
false && echo and-no
false || echo or-yes
true || echo or-no
false && echo no || echo fallback
true && false || echo chain
