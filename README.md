Run:
./build.sh
./launcher.sh <config text file>
./cleanup.sh <config text file>
./cleanFiles.sh

For protocol diagnostics, start each node with MAP_DEBUG=1 in its environment. The
debug log reports application-message sends and receives, snapshot markers and
reports, and halt propagation. For a directly launched node, for example:

MAP_DEBUG=1 ./map-node congig.txt 0

Capture stdout and stderr separately per node when launching remotely so the
node IDs and event order can be compared. Run cleanFiles.sh before a new run to
avoid confusing output files from previous runs with the current snapshots.