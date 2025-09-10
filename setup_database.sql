-- Script para crear la base de datos y tabla de usuarios
-- para la práctica 3 de redes

-- Crear la base de datos
CREATE DATABASE IF NOT EXISTS practica3;
USE practica3;

-- Crear tabla de usuarios
CREATE TABLE IF NOT EXISTS usuarios (
    id INT AUTO_INCREMENT PRIMARY KEY,
    username VARCHAR(50) NOT NULL UNIQUE,
    password VARCHAR(100) NOT NULL,
    fecha_creacion TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Insertar algunos usuarios de prueba
INSERT INTO usuarios (username, password) VALUES 
('admin', 'password'),
('rodrigo', '123456'),
('luis', 'mipassword'),
('usuario1', 'test123'),
('guest', 'guest123');

-- Mostrar los usuarios creados
SELECT id, username, fecha_creacion FROM usuarios;
