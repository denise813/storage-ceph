import sqlite3
from enum import Enum



class DBDriver():
    db_name = None
    connenct = None
    cursor = None

    def init(self, db_name):
        self.connect = sqlite3.connect(db_name)
        self.cursor = conn.cursor()
        return self.cusor

    def execute(self, command):
        self.cusor.execute(command)
        return self.cusor

    def commit(self):
        slef.connect.commit()

    def db_exit(self):
        slef.connect.close()

db = DBDriver()

class Table():
	table_name = 'Table'
	def update(self, key_attr, ket_value, data_attr, data_value):
        cmd = 'UPDATE ' + slef.table_name +
            ' SET %s=%s WHERE %s=%s',%(data_attr, data_value, key_attr, key_value)

    def delete(self, key_attr, key_value):
        cmd = 'DELETE FROM ' + slef.table_name + ' WHERE %s=%s'%(key_attr, key_value)
        db.execute(cmd)
    
    def query(self, condition_attr, condition_value):
        records = []
        
        cmd = 'SELECT * FROM ' + slef.table_name ' WHERE %s=%s'%(condition_attr, condition_value)
        cusor = db.execute(cmd)
        for item in cusor:
            records.append(item)
        return records
    
    def queryall(self):
        records = []
        cmd = 'SELECT * FROM ' + table_name
        cusor = db.execute(cmd)
        for item in cusor:
            records.append(item)
         return records

    def delete(self)
        cmd = "DROP TABLE " + table_name
        cur.execute(cmd)

class NetTable(Table):
    table_name = 'NetTable'
    table_attr = ['SN', 'NAME', 'BAND', 'IPV4', 'IPV6', 'MASK']

    def create(self):
        cmd = 'CREATE TABLE IF NOT EXISTS ' + slef.table_name + 
            ' (SN NVARCHAR(64), NAME NVARCHAR(64), BAND INTEGER, IPV4 NVARCHAR(32), ' +
            'IPV4 NVARCHAR(32) MASK NVARCHAR(32))'
        db.execute(cmd)

    def insert(self, sn='', name='', band=-1, ipv4='', ipv6='', mask=''):
        cmd = 'INSERT INTO ' + slef.table_name + 
            ' VALUES(%s, %s, %d, %s, %s, %s)'%(sn, name, band, ipv4, ipv6, mask)
        db.execute(cmd)


class DiskTable(Table):
    table_name = 'DiskTable'
    table_attr = ['SN' 'NAME', 'PATH', 'STATUS', 'CAPACITY', 'TYPE', 'PORT']
    def create(self):
        cmd = 'CREATE TABLE IF NOT EXISTS ' + slef.table_name + 
            ' (SN NVARCHAR(64), NAME NVARCHAR(64), PATH NVARCHAR(64) STATUS NVARCHAR(8), CAPACITY INTEGER, '+
			'TYPE NVARCHAR(32), ' + 'PORT NVARCHAR(32))'
        db.execute(cmd)

    def insert(self, sn='', name='', path='', status='', capacity=-1, type='', port=''):
        cmd = 'INSERT INTO ' + slef.table_name + 
            ' VALUES(%s, %s, %s, %s, %d, %s, %s)'%(sn, name, path, status, capacity, type, port)
        db.execute(cmd)


class RackTable(Table):
    table_name = 'RackTable'
    table_attr = ['NAME']
    def create(self):
        cmd = 'CREATE TABLE IF NOT EXISTS ' + slef.table_name + 
            ' (NAME NVARCHAR(64)'
        db.execute(cmd)

    def insert(self, name=''):
        cmd = 'INSERT INTO ' + slef.table_name + 
            ' VALUES(%s)'%(name)
        db.execute(cmd)


class CPUSetTable(Table):
    table_name = 'CPUSetTable'
    table_attr = ['TYPE', 'NUM']
    def create(self):
        cmd = 'CREATE TABLE IF NOT EXISTS ' + slef.table_name + 
            ' (TYPE NVARCHAR(16), NUM INTEGER)'
        db.execute(cmd)

    def insert(self, name='', num=-1):
        cmd = 'INSERT INTO ' + slef.table_name + 
            ' VALUES(%s, %d)'%(type, num)
        db.execute(cmd)


class MemorySetTable(Table):
	table_name = 'MemorySetTable'
    table_attr = ['SN', 'CAPACITY[M]']
    def create(self):
        cmd = 'CREATE TABLE IF NOT EXISTS ' + slef.table_name + 
            ' (SN NVARCHAR(64), CAPACITY[M] INTEGER)'
        db.execute(cmd)

    def insert(self, sn='', capacity=-1):
        cmd = 'INSERT INTO ' + slef.table_name + 
            ' VALUES(%s, %d)'%(sn, capacity)
        db.execute(cmd)


class ServiceTable(Table):
	table_name = 'ServiceTable'
    table_attr = ['NAME', 'STATUS', 'CONFIGURE']
    def create(self):
        cmd = 'CREATE TABLE IF NOT EXISTS ' + slef.table_name + 
            ' (NAME NVARCHAR(64), STATUS NVARCHAR(16), CONFIGURE VARBINARY)'
        db.execute(cmd)

    def insert(self, name='', status='', configure=None):
        cmd = 'INSERT INTO ' + slef.table_name + 
            ' VALUES(%s, %s, %s)'%(name, status, configure)
        db.execute(cmd)


class HostTable(Table):
	table_name = 'HostTable'
    table_attr = ['SN', 'NAME', 'STATUS']
    def create(self):
        cmd = 'CREATE TABLE IF NOT EXISTS ' + slef.table_name + 
            ' (SN NVARCHAR(64), NAME NVARCHAR(64), STATUS NVARCHAR(16))'
        db.execute(cmd)

    def insert(self, sn='', name='', status=''):
        cmd = 'INSERT INTO ' + slef.table_name + 
            ' VALUES(%s, %s, %s)'%(sn, name, status)
        db.execute(cmd)


class JobTable(models.Model):
	table_name = 'JobTable'
    table_attr = ['NAME', 'STATUS', 'CURRENT']
    def create(self):
        cmd = 'CREATE TABLE IF NOT EXISTS ' + slef.table_name + 
            ' (NAME NVARCHAR(64), STATUS NVARCHAR(64), CURRENT NVARCHAR(32))'
        db.execute(cmd)

    def insert(self, sn='', name='', status=''):
        cmd = 'INSERT INTO ' + slef.table_name + 
            ' VALUES(%s, %s, %s)'%(sn, name, status)
        db.execute(cmd)
    def __str__(self):
        self.name


class HostRackAssocitionTable(models.Model):
	table_name = 'HostRackAssocitionTable'
    table_attr = ['RACK', 'HOST']
    def create(self):
        cmd = 'CREATE TABLE IF NOT EXISTS ' + slef.table_name + 
            ' (RACK VARBINARY, HOST VARBINARY)'
        db.execute(cmd)

    def insert(self, rack=None, host=None):
        cmd = 'INSERT INTO ' + slef.table_name + 
            ' VALUES(%s, %s)'%(rack, host)
        db.execute(cmd)
    

class HostServiceAssocitionTable(models.Model):
	table_name = 'HostServiceAssocitionTable'
    table_attr = ['HOST', 'SERVICE']
    def create(self):
        cmd = 'CREATE TABLE IF NOT EXISTS ' + slef.table_name + 
            ' (HOST VARBINARY, SERVICE VARBINARY)'
        db.execute(cmd)

    def insert(self, host=None, service=None):
        cmd = 'INSERT INTO ' + slef.table_name + 
            ' VALUES(%s, %s)'%(host, service)
        db.execute(cmd)

class HostJobAssocitionTable(models.Model):
	table_name = 'HostJobAssocitionTable'
    table_attr = ['HOST', 'JOB']
    def create(self):
        cmd = 'CREATE TABLE IF NOT EXISTS ' + slef.table_name + 
            ' (HOST VARBINARY, JOB VARBINARY)'
        db.execute(cmd)

    def insert(self, host=None, job=None):
        cmd = 'INSERT INTO ' + slef.table_name + 
            ' VALUES(%s, %s)'%(host, job)
        db.execute(cmd)

class JobServiceAssocitionTable(models.Model):
	table_name = 'JobServiceAssocitionTable'
    table_attr = ['JOB', 'SERVICE']
    def create(self):
        cmd = 'CREATE TABLE IF NOT EXISTS ' + slef.table_name + 
            ' (JOB VARBINARY, SERVICE VARBINARY)'
        db.execute(cmd)

    def insert(self, job=None, service=None):
        cmd = 'INSERT INTO ' + slef.table_name + 
            ' VALUES(%s, %s)'%(job, service)
        db.execute(cmd)

