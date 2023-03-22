setup
{
 CREATE TABLE accounts (accountid text PRIMARY KEY, balance numeric not null,
   balance2 numeric GENERATED ALWAYS AS (balance * 2) STORED);
 INSERT INTO accounts VALUES ('checking', 600), ('savings', 600);
}

teardown
{
 DROP TABLE accounts;
}

session s1
setup		{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step wb1	{ DELETE FROM accounts WHERE balance = 600 RETURNING *; }
step c1		{ COMMIT; }

session s2
setup		{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step wx2	{ UPDATE accounts SET balance = balance + 450 WHERE accountid = 'checking' RETURNING balance; }
step c2	{ COMMIT; }

session s3
setup		{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step read	{ SELECT * FROM accounts ORDER BY accountid; }
step read_u	{ SELECT * FROM accounts; }

teardown    { COMMIT; }

permutation read_u wx2 wb1 c2 c1 read_u read
