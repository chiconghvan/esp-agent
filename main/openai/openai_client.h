/**
 * ===========================================================================
 * @file openai_client.h
 * @brief Giao tiếp với OpenAI API: Chat Completion và Embedding
 *
 * Module này cung cấp: gọi Chat Completion để phân loại tin nhắn,
 * và gọi Embedding API để tạo vector 384 chiều cho semantic search.
 * ===========================================================================
 */

#ifndef OPENAI_CLIENT_H
#define OPENAI_CLIENT_H

#include "esp_err.h"
#include "config.h"

/**
 * @brief Gọi OpenAI Chat Completion API
 *
 * Gửi system prompt + user message, nhận response text.
 * Dùng cho: phân loại tin nhắn thành action + trích xuất dữ liệu.
 *
 * @param system_prompt System prompt cố định
 * @param user_message Tin nhắn người dùng
 * @param response_buffer Buffer để lưu response (JSON string từ LLM)
 * @param buffer_size Kích thước buffer
 * @return esp_err_t ESP_OK nếu thành công
 */
esp_err_t openai_chat_completion(const char *system_prompt,
                                  const char *user_message,
                                  char *response_buffer,
                                  size_t buffer_size);

/**
 * @brief Gọi OpenAI Embedding API
 *
 * Tạo vector embedding 384 chiều cho một chuỗi text.
 * Dùng cho: tạo vector cho task title hoặc search query.
 *
 * @param text Text cần tạo embedding
 * @param embedding_output Mảng float[EMBEDDING_DIM] để lưu kết quả
 * @param dimensions Số chiều embedding (thường = EMBEDDING_DIM = 384)
 * @return esp_err_t ESP_OK nếu thành công
 */
esp_err_t openai_create_embedding(const char *text,
                                   float *embedding_output,
                                   int dimensions);

#endif /* OPENAI_CLIENT_H */
