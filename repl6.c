#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <stdint.h>
#include <unistd.h>

typedef struct {
	char *buffer;
	size_t buffer_length;
	ssize_t input_length;
} InputBuffer;

typedef enum {
	EXECUTE_SUCCESS,
	EXECUTE_TABLE_FULL
} ExecuteResult;

typedef enum {
	META_COMMAND_SUCCESS,
	META_COMMAND_UNRECOGNIZED_COMMAND
} MetaCommandResult;

typedef enum {
	PREPARE_SUCCESS,
	PREPARE_SYNTAX_ERROR,
	PREPARE_UNRECOGNIZED_STATEMNET
} PrepareResult;

typedef enum {
	STATEMENT_INSERT,
	STATEMENT_SELECT
} StatementType;

typedef enum { NODE_INTERNAL, NODE_LEAF } NodeType;

#define COLUMN_USERNAME_SIZE 32
#define COLUMN_EMAIL_SIZE 255

/* Insert 문장 Row 파싱 후 데이터를 담는 공간 */
typedef struct {
	uint32_t id;
	char username[COLUMN_USERNAME_SIZE];
	char email[COLUMN_EMAIL_SIZE];
} Row;

typedef struct {
	StatementType type;
	Row row_to_insert;
} Statement;

#define size_of_attribute(Struct, Attribute) sizeof(((Struct *)0)->Attribute)

const uint32_t ID_SIZE = size_of_attribute(Row, id);
const uint32_t USERNAME_SIZE = size_of_attribute(Row, username);
const uint32_t EMAIL_SIZE = size_of_attribute(Row, email);
const uint32_t ID_OFFSET = 0;
const uint32_t USERNAME_OFFSET = ID_OFFSET + ID_SIZE;
const uint32_t EMAIL_OFFSET = USERNAME_OFFSET + USERNAME_SIZE;
const uint32_t ROW_SIZE = ID_SIZE + USERNAME_SIZE + EMAIL_SIZE;

/* 커널 페이지 사이즈에 맞춤 */
const uint32_t PAGE_SIZE = 4096;
#define TABLE_MAX_PAGES 100
/* 1페이지에 로우 몇 개가 들어가는지? */
const uint32_t ROWS_PER_PAGE = PAGE_SIZE / ROW_SIZE;
/* 테이블에 들어갈 수 있는 최대 로우 수 */
const uint32_t TABLE_MAX_ROWS = ROWS_PER_PAGE * TABLE_MAX_PAGES;

/*
 * Common Node Header Layout
 */
const uint32_t NODE_TYPE_SIZE = sizeof(uint8_t);
const uint32_t NODE_TYPE_OFFSET = 0;
const uint32_t IS_ROOT_SIZE = sizeof(uint8_t);
const uint32_t IS_ROOT_OFFSET = NODE_TYPE_SIZE;
const uint32_t PARENT_POINTER_SIZE = sizeof(uint32_t);
const uint32_t PARENT_POINTER_OFFSET = IS_ROOT_OFFSET + IS_ROOT_SIZE;
const uint8_t COMMON_NODE_HEADER_SIZE = NODE_TYPE_SIZE + IS_ROOT_SIZE + PARENT_POINTER_SIZE;

/*
 * Leaf Node Header Layout
 */
const uint32_t LEAF_NODE_NUM_CELLS_SIZE = sizeof(uint32_t);
const uint32_t LEAF_NODE_NUM_CELLS_OFFSET = COMMON_NODE_HEADER_SIZE;
const uint32_t LEAF_NODE_HEADER_SIZE = COMMON_NODE_HEADER_SIZE + LEAF_NODE_NUM_CELLS_SIZE;

/*
 * Leaf Node Body Layout
 */
const uint32_t LEAF_NODE_KEY_SIZE = sizeof(uint32_t);
const uint32_t LEAF_NODE_KEY_OFFSET = 0;
const uint32_t LEAF_NODE_VALUE_SIZE = ROW_SIZE;
const uint32_t LEAF_NODE_VALUE_OFFSET = LEAF_NODE_KEY_OFFSET + LEAF_NODE_KEY_SIZE;
const uint32_t LEAF_NODE_CELL_SIZE = LEAF_NODE_KEY_SIZE + LEAF_NODE_VALUE_SIZE;
const uint32_t LEAF_NODE_SPACE_FOR_CELLS = PAGE_SIZE - LEAF_NODE_HEADER_SIZE;
const uint32_t LEAF_NODE_MAX_CELLS = LEAF_NODE_SPACE_FOR_CELLS / LEAF_NODE_CELL_SIZE;

/* 
 * 리프 노트 필드 접근
 */
uint32_t *leaf_node_num_cells(void *node) {
	return node + LEAF_NODE_NUM_CELLS_OFFSET;
}

void *leaf_node_cell(void *node, uint32_t cell_num) {
	return node + LEAF_NODE_HEADER_SIZE + cell_num * LEAF_NODE_CELL_SIZE;
}

uint32_t *leaf_node_key(void *node, uint32_t cell_num) {
	return leaf_node_cell(node, cell_num);
}

void *leaf_node_value(void *node, uint32_t cell_num) {
	return leaf_node_cell(node, cell_num) + LEAF_NODE_KEY_SIZE;
}

void initialize_leaf_node(void *node) { *leaf_node_num_cells(node) = 0; }

/* 페이저 구조체 
   페이지 캐시, 파일에 접근
   테이블 객체는 페이저를 통해 페이지 요청*/
typedef struct {
	int file_descriptor;
	uint32_t file_length;
	void *pages[TABLE_MAX_PAGES];
} Pager;

/* 테이블 구조체 */
typedef struct {
	uint32_t num_rows;
	Pager *pager;
	void *pages[TABLE_MAX_PAGES];
} Table;

/* 커서 구조체 : 테이블 내 위치를 나타내는 객체 */
typedef struct {
	Table *table;
	uint32_t row_num;
	bool end_of_table; // Indicates a position one past the last element 
} Cursor;

void print_row(Row *row) {
	printf("(%d, %s, %s)\n", row->id, row->username, row->email);
}

void serialize_row(Row* source, void* destination) {
	memcpy(destination + ID_OFFSET, &(source->id), ID_SIZE);
	strncpy(destination + USERNAME_OFFSET, source->username, USERNAME_SIZE);
	strncpy(destination + EMAIL_OFFSET, source->email, EMAIL_SIZE);
}

void deserialize_row(void *source, Row *destination) {
	memcpy(&(destination->id), source + ID_OFFSET, ID_SIZE);
	memcpy(&(destination->username), source + USERNAME_OFFSET, USERNAME_SIZE);
	memcpy(&(destination->email), source + EMAIL_OFFSET, EMAIL_SIZE);
}

/* db_open()에서 호출, 데이터베이스 파일을 열고 크기를 추적한다.
   모든 페이지 캐시를 Null로 초기화한다. */
Pager *pager_open(const char *filename) {
	int fd = open(filename,
		O_RDWR |          /* Read/Write mode */
			O_CREAT,  /* 파일이 없으면 생성 */
		S_IWUSR |         /* User write permission */
			S_IRUSR   /* User read permission */
		);
	
	if (fd == -1) {
		printf("Unable to open file\n");
		exit(EXIT_FAILURE);
	}

	off_t file_length = lseek(fd, 0, SEEK_END);

	Pager *pager = malloc(sizeof(Pager));
	pager->file_descriptor = fd;
	pager->file_length = file_length;

	for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
		pager->pages[i] = NULL;
	}

	return pager;
}

Table *db_open(const char* filename) {
	Pager *pager = pager_open(filename);
	uint32_t num_rows = pager->file_length / ROW_SIZE;

	Table *table = (Table *)malloc(sizeof(Table));

	table->pager = pager;
	table->num_rows = num_rows;

	return table;
}

/* get_page()는 캐시 미스를 처리하는 로직을 포함한다. 
   데이터베이스 파일에는 페이지가 순서대로 저장된다고 가정한다.
   예를 들어, 0번째 페이지는 오프셋 0, 1번째 페이지는 오프셋 4096 다음은 8192와 같이
   요청된 페이지가 파일 범위를 벗어나면 해당 페이지는 비어있어야 하므로 메모리를 할당하고 반환한다.
   이 페이지는 나중에 캐시를 디스크에 플러시할 때 파일에 추가된다.*/
void *get_page(Pager *pager, uint32_t page_num) {
	if (page_num > TABLE_MAX_PAGES) {
		printf("Tried to fetch page number out of bounds. %d > %d\n", page_num,
			TABLE_MAX_PAGES);
		exit(EXIT_FAILURE);
	}

	if (pager->pages[page_num] == NULL) {
		// Cache miss. Allocate memory and load from file.
		void *page = malloc(PAGE_SIZE);
		uint32_t num_pages = pager->file_length / PAGE_SIZE;

		// We might save a partial page at the end of the file
		if (page_num <= num_pages) {
			num_pages += 1;
		}

		if (page_num <= num_pages) {
			lseek(pager->file_descriptor, page_num * PAGE_SIZE, SEEK_SET);
			ssize_t bytes_read = read(pager->file_descriptor, page, PAGE_SIZE);
			if (bytes_read == -1) {
				printf("Error reading file: %d\n", errno);
				exit(EXIT_FAILURE);
			}
		}

		pager->pages[page_num] = page;
	}

	return pager->pages[page_num];
}

/* row_num에 맞는 페이지 주소 + byte_offset을 반환 */
void *cursor_value(Cursor *cursor) {
	uint32_t row_num = cursor->row_num;
	uint32_t page_num = row_num / ROWS_PER_PAGE;
	void *page = get_page(cursor->table->pager, page_num);
	uint32_t row_offset = row_num % ROWS_PER_PAGE;
	uint32_t byte_offset = row_offset * ROW_SIZE;
	return page + byte_offset;
}

void pager_flush(Pager *pager, uint32_t page_num, uint32_t size) {
	if (pager->pages[page_num] == NULL) {
		printf("Tried to flush null page\n");
		exit(EXIT_FAILURE);
	}

	off_t offset = lseek(pager->file_descriptor, page_num * PAGE_SIZE, SEEK_SET);

	if (offset == -1) {
		printf("Error seeking: %d\n", errno);
		exit(EXIT_FAILURE);
	}
	
	ssize_t bytes_written = write(pager->file_descriptor, pager->pages[page_num], size);

	if (bytes_written == -1) {
		printf("Error writing: %d\n", errno);
		exit(EXIT_FAILURE);
	}
}

/* 페이지 캐시를 디스크에 저장,
   데이터베이스 파일을 닫음
   Pager 및 Table 구조에 필요한 메모리를 해제 */
void db_close(Table *table) {
	Pager *pager = table->pager;
	uint32_t num_full_pages = table->num_rows / ROWS_PER_PAGE;

	for (uint32_t i = 0; i < num_full_pages; i++) {
		if (pager->pages[i] = NULL) {
			continue;
		}
		pager_flush(pager, i, PAGE_SIZE);
		free(pager->pages[i]);
		pager->pages[i] = NULL;
	}

	// There may be a partial page to write to the end of the file
	// This should not be needed after we switch to a B-tree
	uint32_t num_additional_rows = table->num_rows % ROWS_PER_PAGE;
	if (num_additional_rows > 0) {
		uint32_t page_num = num_full_pages;
		if (pager->pages[page_num] != NULL) {
			pager_flush(pager, page_num, num_additional_rows * ROW_SIZE);
			free(pager->pages[page_num]);
			pager->pages[page_num] = NULL;
		}
	}

	int result = close(pager->file_descriptor);
	if (result == -1) {
		printf("Error closing db file.\n");
		exit(EXIT_FAILURE);
	}
	for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
		void *page = pager->pages[i];
		if (page) {
			free(page);
			pager->pages[i] = NULL;
		}
	}
	free(pager);
	free(table);
}

void free_table(Table *table) {
	for (int i = 0; table->pages[i]; i++) {
		free(table->pages[i]);
	}
	free(table);
}

Cursor *table_start(Table *table) {
	Cursor *cursor = malloc(sizeof(Cursor));
	cursor->table = table;
	cursor->row_num = 0;
	cursor->end_of_table = (table->num_rows == 0);
	
	return cursor;
}

Cursor *table_end(Table *table) {
	Cursor *cursor = malloc(sizeof(Cursor));
	cursor->table = table;
	cursor->row_num = table->num_rows;
	cursor->end_of_table = true;

	return cursor;
}

void cursor_advance(Cursor *cursor) {
	cursor->row_num += 1;
	if (cursor->row_num >= cursor->table->num_rows) {
		cursor->end_of_table = true;
	}
}

InputBuffer *new_input_buffer() {
	InputBuffer *input_buffer = (InputBuffer *)malloc(sizeof(InputBuffer));
	input_buffer->buffer = NULL;
	input_buffer->buffer_length = 0;
	input_buffer->input_length = 0;
	
	return input_buffer;
}

void close_input_buffer(InputBuffer *input_buffer) {
	free(input_buffer->buffer);
	free(input_buffer);
}

MetaCommandResult do_meta_command(InputBuffer *input_buffer, Table *table) {
	if (strcmp(input_buffer->buffer, ".exit") == 0) {
		db_close(table);
		exit(EXIT_SUCCESS);
	} else {
		return META_COMMAND_UNRECOGNIZED_COMMAND;
	}
}

PrepareResult prepare_statement(InputBuffer *input_buffer,
								Statement *statement) {
	if (strncmp(input_buffer->buffer, "insert", 6) == 0) {
		statement->type = STATEMENT_INSERT;
		int args_assigned = sscanf(
			input_buffer->buffer, "insert %d %s %s", &(statement->row_to_insert.id),
			statement->row_to_insert.username, statement->row_to_insert.email
			);
		if (args_assigned < 3) {
			return PREPARE_SYNTAX_ERROR;
		}
		return PREPARE_SUCCESS;
	}
	if (strcmp(input_buffer->buffer, "select") == 0) {
		statement->type = STATEMENT_SELECT;
		return PREPARE_SUCCESS;
	}

	return PREPARE_UNRECOGNIZED_STATEMNET;
}

ExecuteResult execute_insert(Statement *Statement, Table *table) {
	if (table->num_rows >= TABLE_MAX_ROWS) {
		return EXECUTE_TABLE_FULL;
	}

	Row *row_to_insert = &(Statement->row_to_insert);
	Cursor *cursor = table_end(table);

	serialize_row(row_to_insert, cursor_value(cursor));
	table->num_rows += 1;

	return EXECUTE_SUCCESS;
}

ExecuteResult execute_select(Statement *statement, Table *table) {
	Cursor *cursor = table_start(table);

	Row row;
	while (!(cursor->end_of_table)) {
		deserialize_row(cursor_value(cursor), &row);
		print_row(&row);
		cursor_advance(cursor);
	}

	free(cursor);
	return EXECUTE_SUCCESS;
}

ExecuteResult execute_statement(Statement *statement, Table *table) {
	switch (statement->type) {
		case (STATEMENT_INSERT):
			return execute_insert(statement, table);
		case (STATEMENT_SELECT):
			return execute_select(statement, table);
	}
}

void print_prompt() { printf("db >"); }

void read_input(InputBuffer *input_buffer) {
	ssize_t bytes_read = 
		getline(&(input_buffer->buffer), &(input_buffer->buffer_length), stdin);
	if (bytes_read <= 0) {
		printf("Error reading input\n");
		exit(EXIT_FAILURE);
	}

	// Ignore trailing newline
	input_buffer->input_length = bytes_read - 1;
	input_buffer->buffer[bytes_read - 1] = 0;
}

int main(int argc, char *argv[]) {
	if (argc < 2) {
		printf("Must supply a database filename.\n");
		exit(EXIT_FAILURE);
	}

	char *filename = argv[1];
	Table *table = db_open(filename);

	InputBuffer *input_buffer = new_input_buffer();
	while (true) {
		print_prompt();
		read_input(input_buffer);

		/* 메타 커맨드 처리 부분 */
		if (input_buffer->buffer[0] == '.') {
			switch (do_meta_command(input_buffer, table)) {
				case (META_COMMAND_SUCCESS):
					continue;
				case (META_COMMAND_UNRECOGNIZED_COMMAND):
					printf("Unrecognized command '%s'\n", input_buffer->buffer);
					continue;
			}
		}

		Statement statement;

		/* insert, select 판별 */
		switch (prepare_statement(input_buffer, &statement)) {
			case (PREPARE_SUCCESS):
				break;
			case (PREPARE_SYNTAX_ERROR):
				printf("Syntax error. Could not parse statement.\n");
				continue;
			case (PREPARE_UNRECOGNIZED_STATEMNET):
				printf("Unrecognized keyword at start of '%s'.\n",
						input_buffer->buffer);
				continue;
		}

		/* execute */
		switch (execute_statement(&statement, table)) {
			case (EXECUTE_SUCCESS):
				printf("Executed.\n");
				break;
			case (EXECUTE_TABLE_FULL):
				printf("Error: Table full.\n");
				break;
		}
	}
}
