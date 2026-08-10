import os
from typing import Dict, List, Optional, Tuple

import pymysql
from pymysql.cursors import DictCursor


class DatabaseUnavailable(Exception):
    """Raised when MySQL cannot be reached or is not configured."""


class DeviceModeConflict(Exception):
    """Raised when concurrent initialization selected a different device mode."""

    def __init__(self, stored_mode: str):
        super().__init__(stored_mode)
        self.stored_mode = stored_mode


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
                cursor.execute(
                    """
                    CREATE TABLE IF NOT EXISTS conversation_memories (
                        device_id VARCHAR(100) NOT NULL,
                        conversation_id VARCHAR(100) NOT NULL,
                        summary TEXT NOT NULL,
                        last_consolidated_message_id BIGINT UNSIGNED NOT NULL DEFAULT 0,
                        updated_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6)
                            ON UPDATE CURRENT_TIMESTAMP(6),
                        PRIMARY KEY (device_id, conversation_id)
                    ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
                    """
                )
                cursor.execute(
                    """
                    CREATE TABLE IF NOT EXISTS device_profiles (
                        device_id VARCHAR(100) NOT NULL,
                        response_mode ENUM('normal', 'emotion') NOT NULL,
                        created_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
                        updated_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6)
                            ON UPDATE CURRENT_TIMESTAMP(6),
                        PRIMARY KEY (device_id)
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

    def get_conversation_messages(
        self,
        device_id: str,
        conversation_id: str,
        limit: int,
        before_id: Optional[int] = None,
    ) -> Tuple[List[Dict[str, object]], bool]:
        """Return a newest-first database page in chronological display order."""
        self.ensure_schema()
        connection = self._connect()
        try:
            with connection.cursor() as cursor:
                if before_id is None:
                    cursor.execute(
                        """
                        SELECT id, role, content, created_at
                        FROM chat_messages
                        WHERE device_id = %s AND conversation_id = %s
                        ORDER BY id DESC
                        LIMIT %s
                        """,
                        (device_id, conversation_id, limit + 1),
                    )
                else:
                    cursor.execute(
                        """
                        SELECT id, role, content, created_at
                        FROM chat_messages
                        WHERE device_id = %s AND conversation_id = %s AND id < %s
                        ORDER BY id DESC
                        LIMIT %s
                        """,
                        (device_id, conversation_id, before_id, limit + 1),
                    )
                rows = list(cursor.fetchall())
                has_more = len(rows) > limit
                rows = rows[:limit]
                rows.reverse()
                return rows, has_more
        except pymysql.MySQLError as error:
            raise DatabaseUnavailable("MySQL conversation history query failed") from error
        finally:
            connection.close()

    def get_memory(self, device_id: str, conversation_id: str) -> Dict[str, object]:
        self.ensure_schema()
        connection = self._connect()
        try:
            with connection.cursor() as cursor:
                cursor.execute(
                    """
                    SELECT summary, last_consolidated_message_id
                    FROM conversation_memories
                    WHERE device_id = %s AND conversation_id = %s
                    """,
                    (device_id, conversation_id),
                )
                row = cursor.fetchone()
                return row or {"summary": "", "last_consolidated_message_id": 0}
        except pymysql.MySQLError as error:
            raise DatabaseUnavailable("MySQL memory query failed") from error
        finally:
            connection.close()

    def get_response_mode(self, device_id: str):
        """Return the device's bound mode, or None before its first successful chat."""
        self.ensure_schema()
        connection = self._connect()
        try:
            with connection.cursor() as cursor:
                cursor.execute(
                    """
                    SELECT response_mode
                    FROM device_profiles
                    WHERE device_id = %s
                    """,
                    (device_id,),
                )
                row = cursor.fetchone()
                return str(row["response_mode"]) if row else None
        except pymysql.MySQLError as error:
            raise DatabaseUnavailable("MySQL device profile query failed") from error
        finally:
            connection.close()

    def save_turn_with_mode(
        self,
        device_id: str,
        conversation_id: str,
        user_message: str,
        reply: str,
        response_mode: str,
    ) -> None:
        """Atomically bind the first successful mode and persist the completed turn."""
        self.ensure_schema()
        connection = self._connect()
        try:
            connection.begin()
            with connection.cursor() as cursor:
                cursor.execute(
                    """
                    INSERT INTO device_profiles (device_id, response_mode)
                    VALUES (%s, %s)
                    ON DUPLICATE KEY UPDATE device_id = VALUES(device_id)
                    """,
                    (device_id, response_mode),
                )
                cursor.execute(
                    """
                    SELECT response_mode
                    FROM device_profiles
                    WHERE device_id = %s
                    FOR UPDATE
                    """,
                    (device_id,),
                )
                stored_mode = str(cursor.fetchone()["response_mode"])
                if stored_mode != response_mode:
                    connection.rollback()
                    raise DeviceModeConflict(stored_mode)
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
            connection.commit()
        except DeviceModeConflict:
            raise
        except pymysql.MySQLError as error:
            connection.rollback()
            raise DatabaseUnavailable("MySQL mode or chat write failed") from error
        finally:
            connection.close()

    def get_compactable_messages(
        self,
        device_id: str,
        conversation_id: str,
        after_id: int,
        working_memory_messages: int,
        limit: int,
    ) -> List[Dict[str, str]]:
        """Return old, unconsolidated messages while preserving the recent working set."""
        self.ensure_schema()
        connection = self._connect()
        try:
            with connection.cursor() as cursor:
                cursor.execute(
                    """
                    SELECT id, role, content
                    FROM chat_messages
                    WHERE device_id = %s
                      AND conversation_id = %s
                      AND id > %s
                      AND id < COALESCE(
                          (
                              SELECT MIN(id)
                              FROM (
                                  SELECT id
                                  FROM chat_messages
                                  WHERE device_id = %s AND conversation_id = %s
                                  ORDER BY id DESC
                                  LIMIT %s
                              ) AS working_messages
                          ),
                          18446744073709551615
                      )
                    ORDER BY id ASC
                    LIMIT %s
                    """,
                    (
                        device_id,
                        conversation_id,
                        after_id,
                        device_id,
                        conversation_id,
                        working_memory_messages,
                        limit,
                    ),
                )
                return list(cursor.fetchall())
        except pymysql.MySQLError as error:
            raise DatabaseUnavailable("MySQL consolidation query failed") from error
        finally:
            connection.close()

    def save_memory(self, device_id: str, conversation_id: str, summary: str, last_message_id: int) -> None:
        self.ensure_schema()
        connection = self._connect()
        try:
            with connection.cursor() as cursor:
                cursor.execute(
                    """
                    INSERT INTO conversation_memories
                        (device_id, conversation_id, summary, last_consolidated_message_id)
                    VALUES (%s, %s, %s, %s)
                    ON DUPLICATE KEY UPDATE
                        summary = VALUES(summary),
                        last_consolidated_message_id = VALUES(last_consolidated_message_id)
                    """,
                    (device_id, conversation_id, summary, last_message_id),
                )
        except pymysql.MySQLError as error:
            raise DatabaseUnavailable("MySQL memory write failed") from error
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
                cursor.execute(
                    """
                    DELETE FROM conversation_memories
                    WHERE device_id = %s AND conversation_id = %s
                    """,
                    (device_id, conversation_id),
                )
        except pymysql.MySQLError as error:
            raise DatabaseUnavailable("MySQL delete failed") from error
        finally:
            connection.close()
