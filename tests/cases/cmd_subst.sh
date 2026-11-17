echo "[$(echo hello)]"
x=$(echo one; echo two)
echo "$x"
echo "trailing: [$(printf 'a\n\n\n')]"
echo pre$(echo mid)post
echo "$(echo ")")"
echo "$(echo 'single ) paren')"
echo $(echo $(echo nested) "$(echo deeper "$(echo deepest)")")
echo "$(echo a | tr a b)"
y=$(false)
echo "assignment status: $?"
y=$(exit 3)
echo "exit status: $?"
echo "empty: [$(true)]"
echo "pid: $(echo $$)" > out
test "$(cat out)" = "pid: $$" && echo "same \$\$ inside"
echo "$(cd /; pwd)"
pwd > before
cmp -s before out || echo "cd in \$(...) did not leak"
