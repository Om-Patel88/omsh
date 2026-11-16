NAME=world
echo hello $NAME
echo "${NAME}wide"
NAME="two words"
echo "[$NAME]"
unset NAME
echo "[$NAME]"
EMPTY=
sh -c 'echo $#' x $EMPTY $UNSET
sh -c 'echo $#' x "$EMPTY"
echo $ alone and $1x
