---
title: Bulk export YCQL
headerTitle: Bulk export for YCQL
linkTitle: Bulk export
description: Bulk export for YCQL using cassandra-loader and cassandra-unloader.
menu:
  stable:
    identifier: manage-bulk-export-ycql
    parent: manage-bulk-import-export
    weight: 705
type: docs
---

<ul class="nav nav-tabs-alt nav-tabs-yb">
   <li >
    <a href="../bulk-export-ysql/" class="nav-link">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>
  <li >
    <a href="../bulk-export-ycql/" class="nav-link active">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>
</ul>

Bulk export is available for YugabyteDB's [Cassandra-compatible YCQL API](../../../api/ycql/). To export data from a YugabyteDB or an Apache Cassandra table, you can use the [`cassandra-unloader`](https://github.com/yugabyte/cassandra-loader#cassandra-unloader) tool.

A typical workflow is to start with creating a source YugabyteDB table and populate it with data, then exporting the data using the `cassandra-unloader` tool.

## Create source table

The following example represents the schema of the destination YugabyteDB table:

```sql
CREATE KEYSPACE load;
USE load;

CREATE TABLE users(
  user_id varchar,
  score1 double,
  score2 double,
  points int,
  object_id varchar,
   PRIMARY KEY (user_id));
```

## Generate sample data

You can generate data by executing a Python script, as per the following example:

```py
# sample usage:
#  To generate a 10GB (10240 MB) file.
#  % python gen_csv.py <outfile_name> <outfile_size_MB>
#  % python gen_csv.py file01.csv 10240
#
import numpy as np
import uuid
import csv
import os
import sys
outfile    = sys.argv[1] # output file name
outsize_mb = int(sys.argv[2])
print("Outfile = " + outfile)
print("Outfile Size (MB) = " + str(outsize_mb))
chunksize = 10000
with open(outfile, 'ab') as csvfile:
    while (os.path.getsize(outfile)//1024**2) < outsize_mb:
        data = [[uuid.uuid4() for i in range(chunksize)],
                np.random.random(chunksize)*1000,
                np.random.random(chunksize)*50,
                np.random.randint(1000000, size=(chunksize,)),
                [uuid.uuid4() for i in range(chunksize)]]
        csvfile.writelines(['%s,%.6f,%.6f,%i,%s\n' % row for row in zip(*data)])
```

The following shows sample rows generated by the script:

```sh
head file00.csv
```

```output
3399bebc-d2cc-40c6-89d4-26102e08ff61,622.491927,40.262305,658257,44d73f8c-1d3c-424e-8fd2-d316c56b8454
4f362eac-f79f-45f6-b6b1-bd5a81f931dc,141.344278,3.024717,694290,7768b010-8411-490a-b523-88cc3ec53cb5
a24a6587-eea4-4907-ac7f-9f99dcac8f82,345.110599,3.869150,510943,5765d1d3-2855-4dbe-9f11-bb3b8631789f
...
```

To generate five CSV files of approximately 5 GB each, run the following commands:

```sh
python ./gen_csv.py file00.csv 5120 &
python ./gen_csv.py file01.csv 5120 &
python ./gen_csv.py file02.csv 5120 &
python ./gen_csv.py file03.csv 5120 &
python ./gen_csv.py file04.csv 5120 &
```

## Load sample data

[`cassandra-loader`](https://github.com/brianmhess/cassandra-loader) is a general-purpose bulk loader for CQL that supports various types of delimited files, particularly CSV files. For details, review the README file of the [YugabyteDB cassandra-loader fork](https://github.com/yugabyte/cassandra-loader/). Note that `cassandra-loader` requires quotes for collection types (for example, "[1,2,3]" rather than [1,2,3] for lists).

You can install `cassandra-loader` as follows:

```sh
wget https://github.com/yugabyte/cassandra-loader/releases/download/<latest-version>/cassandra-loader
```

```sh
chmod a+x cassandra-loader
```

You can run `cassandra-loader` and queue up the files for upload one at a time, as follows:

```sh
./cassandra-loader \
    -schema "load.users(user_id, score1, score2, points, object_id)" \
    -boolStyle 1_0 \
    -numFutures 1000 \
    -rate 10000 \
    -queryTimeout 65 \
    -numRetries 10 \
    -progressRate 200000 \
    -host <clusterNodeIP> \
    -f file01.csv
```

For additional options, refer to [cassandra-loader options](https://github.com/yugabyte/cassandra-loader#options).

## Export data

You can install `cassandra-unloader` as follows:

```sh
wget https://github.com/brianmhess/cassandra-loader/releases/download/<latest-version>/cassandra-unloader
```

```sh
chmod a+x cassandra-unloader
```

You can run `cassandra-unloader` as follows:

```sh
./cassandra-unloader \
   -schema "load.users(user_id, score1, score2, points, object_id)" \
   -boolStyle 1_0 \
   -host <clusterNodeIP> \
   -f outfile.csv
```

For additional options, refer to [cassandra-unloader options](https://github.com/yugabyte/cassandra-loader#cassandra-unloader).

## Best practices

Be sure to always specify the time zone, as it is not added to the default timestamp formats when using the `cassandra-loader` and `cassandra-unloader` utilities. Timestamps must be exported and imported in the same format, including the time zone. For example, `yyyy-MM-dd HH:mm:ss.SSSZ` and `yyyy-MM-dd HH:mm:ss.SSSXXX`.

It is recommended to use tab character as delimiter on JSONB columns, as the default delimiter, comma ( `,` ) does not work with these type of columns. For example, `-delim $'\t'`.