! false
echo $?
! true
echo $?
! sh -c 'exit 3'
echo $?
'!' x 2>/dev/null
echo $?
