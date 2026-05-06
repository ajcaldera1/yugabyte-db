--
-- ddl_instead_of: ProcessUtility_hook rewrite before ddl_command_start
--

set client_min_messages = warning;

create extension ddl_instead_of;

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

select ddl_instead_of.add_rule(
	't_rewrite',
	'CREATE TABLE',
	'ddl_io_test_rewrite(text,text)'::regprocedure,
	10);

create table ddl_io_rewrite_table (i int);

select string_agg(a.attname, ',' order by a.attnum) as cols
from pg_attribute a
where a.attrelid = 'ddl_io_rewrite_table'::regclass
	and a.attnum > 0
	and not a.attisdropped;

drop table ddl_io_rewrite_table;

create table ddl_io_plain (x int);

select string_agg(a.attname, ',' order by a.attnum) as cols
from pg_attribute a
where a.attrelid = 'ddl_io_plain'::regclass
	and a.attnum > 0
	and not a.attisdropped;

drop table ddl_io_plain;

select ddl_instead_of.drop_rule('t_rewrite');

drop function public.ddl_io_test_rewrite(text, text);

drop extension ddl_instead_of;
