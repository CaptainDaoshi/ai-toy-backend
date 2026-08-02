import os
from typing import Dict, List

import pymysql
from pymysql.cursors import DictCursor


class DatabaseUnavailable(Exception):
    """Raised when MySQL cannot be reached or is not configured."""


class ChatDatabase:
    def __init__(self):
        self.host = os.getenv("MYSQL_HOST", "")
        self.port = int(os.getenv("MYSQL_PORT", "3306"))
        self.user = os.getenv("MYSQL_USER", "")
        self.password = os.getenv("MYSQL_PASSWORD", "")
        self.database = os.getenv("MYSQL_DATABASE", "")
        self.connect_timeout = int(os.getenv("MYSQL_CONNECT_TIMEOUT", "5"))
        self.schema_initialized = False

    @property
    def configured(self) -> bool:
        return bool(self.host and self.user and self.database)

    def _connect(self):
        if not self.configured:
            raise DatabaseUnavailable("MySQL is not configured")
        try:
            return pymysql.connect(
                host=self.host,
                port=self.port,
                user=self.user,
                password=self.password,
                database=self.database,
                charset="utf8mb4",
                cursorclass=DictCursor,
                autocommit=True,
                connect_timeout=self.connect_timeout,
            )
        except pymysql.MySQLError as error:
            raise DatabaseUnavailable("MySQL is unavailable") from error

    def ensure_schema(self) -> None:
        if self.schema_initialized:
            return
        connection = self._connect()
        try:
            with connection.cursor() as cursor:
                cursor.execute(
                    """
                    CREATE TABLE IF NOT EXISTS chat_messages (
                        id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
                        device_id VARCHAR(100) NOT NULL,
                        conversation_id VARCHAR(100) NOT NULL,
                        role ENUM('user', 'assistant') NOT NULL,
                        content TEXT NOT NULL,
                        created_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
                        PRIMARY KEY (id),
                        INDEX idx_chat_messages_conversation (device_id, conversation_id, id)
                    ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
                    """
                )
            self.schema_initialized = True
        except pymysql.MySQLError as error:
            raise DatabaseUnavailable("MySQL schema initialization failed") from error
        finally:
            connection.close()

    def get_history(self, device_id: str, conversation_id: str, limit: int) -> List[Dict[str, str]]:
        self.ensure_schema()
        connection = self._connect()
        try:
            with connection.cursor() as cursor:
                cursor.execute(
                    """
                    SELECT role, content
                    FROM (
                        SELECT id, role, content
                        FROM chat_messages
                        WHERE device_id = %s AND conversation_id = %s
                        ORDER BY id DESC
                        LIMIT %s
                    ) AS recent_messages
                    ORDER BY id ASC
                    """,
                    (device_id, conversation_id, limit),
                )
                return list(cursor.fetchall())
        except pymysql.MySQLError as error:
            raise DatabaseUnavailable("MySQL query failed") from error
        finally:
            connection.close()

    def save_turn(self, device_id: str, conversation_id: str, user_message: str, reply: str) -> None:
        self.ensure_schema()
        connection = self._connect()
        try:
            with connection.cursor() as cursor:
                cursor.executemany(
                    """
                    INSERT INTO chat_messages (device_id, conversation_id, role, content)
                    VALUES (%s, %s, %s, %s)
                    """,
                    [
                        (device_id, conversation_id, "user", user_message),
                        (device_id, conversation_id, "assistant", reply),
                    ],
                )
        except pymysql.MySQLError as error:
            raise DatabaseUnavailable("MySQL write failed") from error
        finally:
            connection.close()

    def clear_conversation(self, device_id: str, conversation_id: str) -> None:
        self.ensure_schema()
        connection = self._connect()
        try:
            with connection.cursor() as cursor:
                cursor.execute(
                    """
                    DELETE FROM chat_messages
                    WHERE device_id = %s AND conversation_id = %s
                    """,
                    (device_id, conversation_id),
                )
        except pymysql.MySQLError as error:
            raise DatabaseUnavailable("MySQL delete failed") from error
        finally:
            connection.close()
