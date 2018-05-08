Bitcoin Candy version 0.17.0 is available now.


This release includes the following features and fixes:

- Increase the maximum blocksize to 32,000,000 bytes
- Increase the OP_RETURN size to 600 bytes
- Re-enabling of several opcodes: OP_CAT, OP_AND, OP_OR, OP_XOR, OP_DIV, OP_MOD
- New opcodes: OP_SPLIT to replace OP_SUBSTR, OP_NUM2BIN, OP_BIN2NUM
- DAA optimization to make cdy network responds faster when hashrate fluctuating violently.



Some modifications port from Bitcoin Cash such as : 
- Remove unused plTxnReplaced parameter to AcceptToMemoryPool
- Compute script flags based on the chain tip rather than the block we are connecting
- Prepare a flag for charging for excessive utxo creation
- Make sure compact blocks work when there are more than 65536 transactions in a block.
- Changing maximum length of incoming protocol messages from 32MB to 4MB (no message over 4 MB is acceptable).


New DAA will be activted after block height 591007 and other consensus rules will take effect after MTP time of 1526194800(Sunday, 13-May-18 07:00:00 UTC).
