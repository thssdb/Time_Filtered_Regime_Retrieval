"""Open a MySQL-mode OceanBase connection to a caller-provided database."""
import os

import pymysql


def connect(database):
    options = {"host": os.environ["OB_HOST"], "port": int(os.environ.get("OB_PORT", "2881")),
               "user": os.environ["OB_USER"], "password": os.environ.get("OB_PASSWORD", ""),
               "database": database, "autocommit": True, "binary_prefix": True,
               "charset": "utf8mb4", "read_timeout": 120, "write_timeout": 120}
    if socket := os.environ.get("OB_UNIX_SOCKET"):
        options["unix_socket"] = socket
    if ca := os.environ.get("OB_SSL_CA"):
        options.update(ssl_ca=ca, ssl_verify_cert=True, ssl_verify_identity=True)
    return pymysql.connect(**options)
