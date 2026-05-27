--
-- ddl_instead_of: ProcessUtility_hook rewrite before ddl_command_start
--

set client_min_messages = warning;

create extension ddl_instead_of;

-- validate_handler rejects a function with the wrong signature
create function public.ddl_io_bad_handler(x int) returns text language sql as $$ select null::text $$;
select ddl_instead_of.validate_handler('ddl_io_bad_handler(int)'::regprocedure);
drop function public.ddl_io_bad_handler(int);

-- validate_handler accepts the correct (text, text) -> text signature
create or replace function public.ddl_io_test_rewrite(cmd text, stmt text)
returns text
language plpgsql
as $body$
begin
	if cmd = 'CREATE TABLE' and position('ddl_io_rewrite_table' in stmt) > 0 then
		return 'CREATE TABLE ddl_io_rewrite_table (i int, j int default 42)';
	end if;
	return null;
end;
$body$;

select ddl_instead_of.validate_handler('ddl_io_test_rewrite(text,text)'::regprocedure);

select ddl_instead_of.add_rule(
	't_rewrite',
	'CREATE TABLE',
	'ddl_io_test_rewrite(text,text)'::regprocedure,
	10);

-- rewrite path: handler replaces the statement, extra column j appears
create table ddl_io_rewrite_table (i int);

select string_agg(a.attname, ',' order by a.attnum) as cols
from pg_attribute a
where a.attrelid = 'ddl_io_rewrite_table'::regclass
	and a.attnum > 0
	and not a.attisdropped;

drop table ddl_io_rewrite_table;

-- no-rewrite path: non-matching name passes through unchanged
create table ddl_io_plain (x int);

select string_agg(a.attname, ',' order by a.attnum) as cols
from pg_attribute a
where a.attrelid = 'ddl_io_plain'::regclass
	and a.attnum > 0
	and not a.attisdropped;

drop table ddl_io_plain;

-- drop_rule on a missing name should raise an error
select ddl_instead_of.drop_rule('no_such_rule');

select ddl_instead_of.drop_rule('t_rewrite');

-- confirm the rule is gone
select count(*) from ddl_instead_of.intercept_rule where rule_name = 't_rewrite';

drop function public.ddl_io_test_rewrite(text, text);

drop extension ddl_instead_of;
