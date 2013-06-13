goog.provide("rethinkdb.query")

goog.require("rethinkdb.base")
goog.require("rethinkdb.ast")

rethinkdb.expr = ar (val) ->
    if val is undefined
        throw new RqlDriverError "Cannot wrap undefined with r.expr()."

    else if val instanceof TermBase
        val
    else if val instanceof Function
        new Func {}, val
    else if goog.isArray val
        new MakeArray {}, val...
    else if goog.isObject val
        new MakeObject val
    else
        new DatumTerm val

rethinkdb.js = aropt (jssrc, opts) -> new JavaScript opts, jssrc

rethinkdb.error = varar 0, 1, (args...) -> new UserError {}, args...

rethinkdb.row = new ImplicitVar {}

rethinkdb.table = aropt (tblName, opts) -> new Table opts, tblName

rethinkdb.db = ar (dbName) -> new Db {}, dbName

rethinkdb.dbCreate = ar (dbName) -> new DbCreate {}, dbName
rethinkdb.dbDrop = ar (dbName) -> new DbDrop {}, dbName
rethinkdb.dbList = ar () -> new DbList {}

rethinkdb.tableCreate = aropt (tblName, opts) -> new TableCreate opts, tblName
rethinkdb.tableDrop = ar (tblName) -> new TableDrop {}, tblName
rethinkdb.tableList = ar () -> new TableList {}

rethinkdb.do = varar 1, null, (args...) ->
    new FunCall {}, funcWrap(args[-1..][0]), args[...-1]...

rethinkdb.branch = ar (test, trueBranch, falseBranch) -> new Branch {}, test, trueBranch, falseBranch

rethinkdb.count =              {'COUNT': true}
rethinkdb.sum   = ar (attr) -> {'SUM': attr}
rethinkdb.avg   = ar (attr) -> {'AVG': attr}

rethinkdb.asc = (attr) -> new Asc {}, attr
rethinkdb.desc = (attr) -> new Desc {}, attr

rethinkdb.eq = varar 2, null, (args...) -> new Eq {}, args...
rethinkdb.ne = varar 2, null, (args...) -> new Ne {}, args...
rethinkdb.lt = varar 2, null, (args...) -> new Lt {}, args...
rethinkdb.le = varar 2, null, (args...) -> new Le {}, args...
rethinkdb.gt = varar 2, null, (args...) -> new Gt {}, args...
rethinkdb.ge = varar 2, null, (args...) -> new Ge {}, args...
rethinkdb.or = varar 2, null, (args...) -> new Any {}, args...
rethinkdb.and = varar 2, null, (args...) -> new All {}, args...

rethinkdb.not = ar (x) -> new Not {}, x

rethinkdb.add = varar 2, null, (args...) -> new Add {}, args...
rethinkdb.sub = varar 2, null, (args...) -> new Sub {}, args...
rethinkdb.mul = varar 2, null, (args...) -> new Mul {}, args...
rethinkdb.div = varar 2, null, (args...) -> new Div {}, args...
rethinkdb.mod = ar (a, b) -> new Mod {}, a, b

rethinkdb.typeOf = ar (val) -> new TypeOf {}, val
rethinkdb.info = ar (val) -> new Info {}, val
