# subgraph
subgraph count

### To compile the code, run

    ./compile-nec-ncc.sh

This will generate the executable binary sc-nec-ncc.bin

### To run the program:

    /opt/nec/ve/bin/ve_exec -N 1 ./sc-nec-ncc.bin web-Google.csc.data u3-1.fascia 1 8 1 0 1 1
    
dataset file (web-Google.csc.data) and template file (u3-1.fascia) need to point to the right location where the files are actually located.
